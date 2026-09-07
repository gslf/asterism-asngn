"""Actual Git loose/packed refs, worktree commits, detach and SHA-256 identity."""
from pathlib import Path
import os
import subprocess
import sys
import tempfile

probe = str(Path(sys.argv[1]).resolve())


def run(command, cwd):
    result = subprocess.run(command, cwd=cwd, text=True, stdout=subprocess.PIPE,
                            stderr=subprocess.PIPE, timeout=30)
    if result.returncode:
        raise AssertionError(f'{command!r} exited {result.returncode}\n'
                             f'stdout:\n{result.stdout}\nstderr:\n{result.stderr}')
    return result.stdout.strip()


def observe(root, branch):
    result = run([probe, str(root)], root).splitlines()
    assert result[0] == 'ASNGN_OK', result
    assert result[1] == run(['git', 'rev-parse', 'HEAD'], root), result
    assert result[2] == branch, result
    assert len(result[3]) == 64, result
    return result[3]


def observe_windows_aliases(root, branch, expected):
    if os.name != 'nt':
        return
    import ctypes
    from ctypes import wintypes
    kernel = ctypes.WinDLL('kernel32', use_last_error=True)
    for name in ('GetLongPathNameW', 'GetShortPathNameW'):
        convert = getattr(kernel, name)
        convert.argtypes = [wintypes.LPCWSTR, wintypes.LPWSTR, wintypes.DWORD]
        convert.restype = wintypes.DWORD
        buffer = ctypes.create_unicode_buffer(32768)
        length = convert(str(root), buffer, len(buffer))
        assert 0 < length < len(buffer), (name, ctypes.get_last_error())
        # Fingerprints include the supplied root spelling; compare Git identity
        # through observe, not fingerprints belonging to different spellings.
        observe(Path(buffer.value), branch)
    assert observe(root, branch) == expected


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
        observe_windows_aliases(job, 'feature/review', before)
        (job / 'source.c').write_text('revised\n')
        run(['git', 'commit', '-qam', 'revised'], job)
        assert observe(job, 'feature/review') != before
        run(['git', 'pack-refs', '--all', '--prune'], main)
        observe(main, 'main')
        observe(job, 'feature/review')
        run(['git', 'checkout', '--detach'], job)
        observe(job, 'detached')
print('Real Git SHA-1/SHA-256 worktree identities passed')
