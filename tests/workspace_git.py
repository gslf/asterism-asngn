"""Actual Git loose/packed refs, worktree commits, detach and SHA-256 identity."""
from pathlib import Path
import subprocess
import sys
import tempfile

probe = str(Path(sys.argv[1]).resolve())


def run(command, cwd):
    return subprocess.run(command, cwd=cwd, text=True, stdout=subprocess.PIPE,
                          stderr=subprocess.PIPE, timeout=30, check=True).stdout.strip()


def observe(root, branch):
    result = run([probe, str(root)], root).splitlines()
    assert result[0] == 'ASNGN_OK', result
    assert result[1] == run(['git', 'rev-parse', 'HEAD'], root), result
    assert result[2] == branch, result
    assert len(result[3]) == 64, result
    return result[3]


with tempfile.TemporaryDirectory(prefix='asterism-git-') as td:
    root = Path(td)
    for algorithm in ('sha1', 'sha256'):
        main, job = root / algorithm, root / (algorithm + '-job')
        main.mkdir()
        run(['git', 'init', '-b', 'main', '--object-format=' + algorithm], main)
        run(['git', 'config', 'user.name', 'Fixture'], main)
        run(['git', 'config', 'user.email', 'fixture@example.invalid'], main)
        (main / 'source.c').write_text('initial\n')
        run(['git', 'add', '.'], main)
        run(['git', 'commit', '-qm', 'initial'], main)
        observe(main, 'main')
        run(['git', 'worktree', 'add', '-b', 'feature/review', str(job)], main)
        before = observe(job, 'feature/review')
        (job / 'source.c').write_text('revised\n')
        run(['git', 'commit', '-qam', 'revised'], job)
        assert observe(job, 'feature/review') != before
        run(['git', 'pack-refs', '--all', '--prune'], main)
        observe(main, 'main')
        observe(job, 'feature/review')
        run(['git', 'checkout', '--detach'], job)
        observe(job, 'detached')
print('Real Git SHA-1/SHA-256 worktree identities passed')
