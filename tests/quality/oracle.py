"""Protected verification in a fresh fixture, with no writable acceptance inputs."""
from pathlib import Path
import os
import shutil
import subprocess


def isolated(command: list[str], root: Path) -> subprocess.CompletedProcess:
    """Linux evaluator: only toolchains, fixture, scratch and build output exist."""
    if not shutil.which('bwrap'):
        return subprocess.CompletedProcess(command, 125, 'bubblewrap unavailable')
    build = root / 'build'
    build.mkdir(exist_ok=True)
    argv = ['bwrap', '--unshare-all', '--die-with-parent', '--new-session']
    for path in ('/usr', '/bin', '/lib', '/lib64'):
        if Path(path).exists():
            argv += ['--ro-bind', path, path]
    argv += ['--proc', '/proc', '--dev', '/dev', '--tmpfs', '/tmp',
             '--dir', '/etc', '--ro-bind', str(root), '/workspace',
             '--bind', str(build), '/workspace/build', '--chdir', '/workspace',
             '--clearenv', '--setenv', 'PATH', '/usr/bin:/bin',
             '--setenv', 'HOME', '/tmp', '--setenv', 'LC_ALL', 'C', '--']
    # Resolve evaluator-owned executables; never inherit a user startup module.
    command = ['/usr/bin/python3' if c == 'python3' else c for c in command]
    try:
        return subprocess.run(argv + command, text=True, stdout=subprocess.PIPE,
                              stderr=subprocess.STDOUT, timeout=180, check=False)
    except (OSError, subprocess.TimeoutExpired) as error:
        return subprocess.CompletedProcess(command, 125, str(error))


def hidden_checks(task: dict, root: Path) -> list[list[str]]:
    """Materialized after patch application, never in the agent's workspace."""
    if task['name'] == 'python_config_merge':
        (root / '.oracle.py').write_text('''import sys
sys.path.insert(0, "/workspace" if __file__.startswith("/workspace/") else str(__import__("pathlib").Path(__file__).parent))
from config_merge import merge_defaults
base = {'a': {'b': {'x': 1, 'y': 2}}, 'list': [1]}
override = {'a': {'b': {'y': 3}}, 'extra': [4]}
r = merge_defaults(base, override)
assert r == {'a': {'b': {'x': 1, 'y': 3}}, 'list': [1], 'extra': [4]}
r['list'].append(9)
r['extra'].append(9)
assert base['list'] == [1] and override['extra'] == [4]
assert merge_defaults({'a': {'b': 1}}, {'a': None}) == {'a': None}
''', encoding='utf-8')
        return [['python3', '-I', '-B', '.oracle.py']]
    (root / '.oracle.c').write_text('''#include "range.h"
#include <assert.h>
int main(void) {
  int xs[] = {-8, 3, 2, 999};
  assert(range_sum(xs, 3) == -3);
  assert(range_sum(xs, 1) == -8);
  assert(range_sum(xs, 0) == 0);
  assert(range_sum((void *)0, 0) == 0);
  return 0;
}
''', encoding='utf-8')
    return [['cc', '-std=c99', '-Wall', '-Werror', '-I.', 'range.c', '.oracle.c',
             '-o', 'build/oracle'], ['./build/oracle']]
