"""Bounded POSIX evaluation processes; never infer success from partial output."""
from dataclasses import dataclass
import os
import selectors
import subprocess
import threading
import time

from metrics import monitor_rss, terminate

OUTPUT_LIMIT = 4 * 1024 * 1024


@dataclass
class Result:
    args: list[str]
    returncode: int | None = None
    stdout: str = ''
    error: str | None = None
    peak_tree_rss_kb: int | None = None

    @property
    def ok(self):
        return self.returncode == 0 and self.error is None


def run(command, cwd, timeout=180, *, output_limit=OUTPUT_LIMIT, measure=False,
        pass_fds=()):
    """Retain at most output_limit bytes, including stderr, until one deadline.

    The process group is closed even when its leader exits first. Production
    verification additionally uses a PID namespace; a process group alone cannot
    contain a descendant that creates another session.
    """
    result = Result(command)
    if os.name != 'posix':
        result.error = 'unsupported_platform'
        return result
    if timeout <= 0 or output_limit < 1:
        raise ValueError('timeout and output limit must be positive')
    deadline = time.monotonic() + timeout
    try:
        proc = subprocess.Popen(command, cwd=cwd, stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL,
                                start_new_session=True, bufsize=0, pass_fds=pass_fds)
    except OSError as error:
        result.error, result.stdout = 'startup_error', str(error)
        return result
    memory, output = {}, bytearray()
    group_closed = False
    monitor = None
    if measure:
        monitor = threading.Thread(target=monitor_rss, args=(proc, memory), daemon=True)
        monitor.start()
    try:
        with selectors.DefaultSelector() as selector:
            os.set_blocking(proc.stdout.fileno(), False)
            selector.register(proc.stdout, selectors.EVENT_READ)
            while selector.get_map() or proc.poll() is None:
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    result.error = 'deadline'
                    break
                if proc.poll() is not None and not group_closed:
                    terminate(proc)  # Do not wait for an orphan to close stdout.
                    group_closed = True
                for key, _ in selector.select(min(remaining, .05)):
                    data = os.read(key.fd, min(65536, output_limit - len(output) + 1))
                    if not data:
                        selector.unregister(key.fileobj)
                    elif len(output) + len(data) > output_limit:
                        output.extend(data[:output_limit - len(output)])
                        result.error = 'output_limit'
                        break
                    else:
                        output.extend(data)
                if result.error:
                    break
    except OSError:
        result.error = 'stream_error'
    finally:
        if not group_closed:
            terminate(proc)
        proc.stdout.close()
        result.returncode = proc.wait()
        if monitor:
            monitor.join(timeout=1)
        result.peak_tree_rss_kb = memory.get('peak_tree_rss_kb')
    result.stdout = output.decode('utf-8', errors='replace')
    return result
