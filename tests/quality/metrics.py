"""Measurements with explicit sampling limits; no inferred model confidence."""
import math
import os
import re
import signal
import time
from pathlib import Path


def process_tree_rss(pid):
    """Sum sampled Linux RSS, including descendants (shared pages may repeat)."""
    pending, seen, total = [pid], set(), 0
    while pending:
        current = pending.pop()
        if current in seen:
            continue
        seen.add(current)
        root = Path(f'/proc/{current}')
        try:
            match = re.search(r'^VmRSS:\s+(\d+)\s+kB',
                              (root / 'status').read_text(), re.M)
            if match:
                total += int(match[1])
            # Children are attached to the thread which spawned them.
            for task in (root / 'task').iterdir():
                pending.extend(map(int, (task / 'children').read_text().split()))
        except (OSError, ValueError):
            pass  # A child can disappear between samples.
    return total


def monitor_rss(proc, result):
    if not Path('/proc/self/status').exists():
        result['peak_tree_rss_kb'] = None
        return
    peak = 0
    while proc.poll() is None:
        peak = max(peak, process_tree_rss(proc.pid))
        time.sleep(.05)
    result['peak_tree_rss_kb'] = peak


def terminate(proc):
    """The caller starts a new session so cancellation reaches tool children."""
    if os.name == 'posix':
        try:
            os.killpg(proc.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
    elif proc.poll() is None:
        proc.kill()


def interval(successes, count):
    """95% Wilson interval for Bernoulli trials; correlated tasks need caution."""
    if not count:
        return None
    z = 1.959963984540054
    p, z2 = successes / count, z * z
    center = (p + z2 / (2 * count)) / (1 + z2 / count)
    radius = z * math.sqrt(p * (1 - p) / count + z2 / (4 * count**2)) / (1 + z2 / count)
    return [max(0, center - radius), min(1, center + radius)]


def percentile(values, fraction):
    values = sorted(values)
    if not values:
        return None
    return values[max(0, math.ceil(len(values) * fraction) - 1)]
