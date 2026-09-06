"""Closed Linux verification profile with an out-of-process lifecycle receipt."""
import json
from pathlib import Path
import shutil
import tempfile

from checks import unique_fields
from command import Result, run

STATUS_LIMIT = 16384


def status_error(data: bytes, exit_code: int | None) -> str | None:
    """Require bubblewrap's terminal exec receipt, not text from the payload.

    A child-pid record alone does not establish that setup or exec succeeded.
    Missing terminal state leaves execution unknown, including monitor failure.
    """
    if len(data) > STATUS_LIMIT:
        return 'sandbox_status_limit'
    try:
        records = [json.loads(line, object_pairs_hook=unique_fields)
                   for line in data.decode('utf-8').splitlines()]
    except (ValueError, TypeError, RecursionError):
        return 'sandbox_status_invalid'
    if not records:
        return 'sandbox_status_incomplete'
    first = records[0]
    if (not isinstance(first, dict) or 'child-pid' not in first or
            any(type(value) is not int or value <= 0 for value in first.values()) or
            any(key != 'child-pid' and not key.endswith('-namespace') for key in first)):
        return 'sandbox_status_invalid'
    if len(records) == 1:
        return 'sandbox_status_incomplete'
    terminal = records[1]
    if (len(records) != 2 or not isinstance(terminal, dict) or
            set(terminal) != {'exit-code'} or type(terminal['exit-code']) is not int or
            not 0 <= terminal['exit-code'] <= 255 or terminal['exit-code'] != exit_code):
        return 'sandbox_status_invalid'
    return None


def isolated(command: list[str], root: Path) -> Result:
    """Only toolchains, fixture, scratch and build output exist in the sandbox."""
    executable = shutil.which('bwrap')
    if not executable:
        return Result(command, error='sandbox_unavailable')
    result = Result(command)
    try:
        build = root / 'build'
        build.mkdir(exist_ok=True)
        # This unlinked descriptor belongs to the monitor. Bubblewrap closes it
        # in the sandbox child before exec; the payload cannot emit this receipt.
        with tempfile.TemporaryFile() as status:
            fd = status.fileno()
            argv = [executable, '--unshare-all', '--die-with-parent', '--new-session',
                    '--json-status-fd', str(fd)]
            for path in ('/usr', '/bin', '/lib', '/lib64'):
                if Path(path).exists():
                    argv += ['--ro-bind', path, path]
            argv += ['--proc', '/proc', '--dev', '/dev', '--tmpfs', '/tmp',
                     '--dir', '/etc', '--ro-bind', str(root), '/workspace',
                     '--bind', str(build), '/workspace/build', '--chdir', '/workspace',
                     '--clearenv', '--setenv', 'PATH', '/usr/bin:/bin',
                     '--setenv', 'HOME', '/tmp', '--setenv', 'LC_ALL', 'C', '--']
            # Resolve evaluator-owned executables; never inherit a startup module.
            command = ['/usr/bin/python3' if c == 'python3' else c for c in command]
            result = run(argv + command, root, pass_fds=(fd,))
            status.seek(0)
            error = status_error(status.read(STATUS_LIMIT + 1), result.returncode)
            result.error = result.error or error
    except OSError:
        result.error = result.error or 'sandbox_status_io'
    return result
