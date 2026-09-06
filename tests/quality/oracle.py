"""Protected verification in a fresh fixture, with no writable acceptance inputs."""
from pathlib import Path
import shutil
from checks import Check
from command import Result, run


def isolated(command: list[str], root: Path) -> Result:
    """Linux evaluator: only toolchains, fixture, scratch and build output exist."""
    if not shutil.which('bwrap'):
        return Result(command, error='sandbox_unavailable')
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
    return run(argv + command, root)


def protected_checks(task: dict, root: Path) -> list[Check]:
    """Materialize completion probes only after implementation patch admission."""
    if task['name'] == 'python_config_merge':
        shutil.copyfile(Path(__file__).with_name('check_runner.py'), root / '.runner.py')
        (root / '_oracle.py').write_text('''import unittest
from config_merge import merge_defaults
class ProtectedMergeTests(unittest.TestCase):
    def test_deep_merge(self):
        base = {'a': {'b': {'x': 1, 'y': 2}}, 'list': [1]}
        got = merge_defaults(base, {'a': {'b': {'y': 3}}, 'extra': [4]})
        self.assertEqual(got, {'a': {'b': {'x': 1, 'y': 3}}, 'list': [1], 'extra': [4]})

    def test_detached_inputs(self):
        base, override = {'list': [1]}, {'extra': [4]}
        got = merge_defaults(base, override)
        got['list'].append(9)
        got['extra'].append(9)
        self.assertEqual((base, override), ({'list': [1]}, {'extra': [4]}))

    def test_null_replacement(self):
        self.assertEqual(merge_defaults({'a': {'b': 1}}, {'a': None}), {'a': None})
''', encoding='utf-8')
        checks = []
        for module, expected in [('test_config_merge', 2), ('_oracle', 3)]:
            check = Check([], expected)
            check.command = ['python3', '-I', '-B', '.runner.py', module, check.identity]
            checks.append(check)
        return checks
    check = Check(['./build/oracle'], 4)
    # Explicit conditions remain active even if a compiler defines NDEBUG.
    (root / '.oracle.c').write_text('''#include "range.h"
#include <stdio.h>
int main(void) {
  int xs[] = {-8, 3, 2, 999};
  if (range_sum(xs, 3) != -3 || range_sum(xs, 1) != -8 ||
      range_sum(xs, 0) != 0 || range_sum((void *)0, 0) != 0) return 1;
  puts("ASTERISM_CHECK {\\"schema\\":1,\\"action_id\\":\\"IDENTITY\\","
       "\\"collected\\":4,\\"executed\\":4,\\"skipped\\":0,\\"failures\\":0,\\"errors\\":0}");
  return 0;
}
'''.replace('IDENTITY', check.identity), encoding='utf-8')
    return [*[Check(command) for command in task['verify']],
            Check(['cc', '-std=c99', '-Wall', '-Werror', '-I.', 'range.c',
                   '.oracle.c', '-o', 'build/oracle']), check]
