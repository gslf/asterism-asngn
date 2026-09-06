"""Compare two replay probes on one synthetic schema-2 journal; no inference."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import statistics
import subprocess
import sys
import tempfile
import time
import uuid


def fixture(path, count):
    day = int(time.time()) // 86400
    with path.open('wb') as target:
        for settled in (False, True):
            for i in range(count) if not settled else reversed(range(count)):
                known = settled and i % 2 == 0
                record = dict(schema=2, id=str(uuid.UUID(int=i + 1, version=4)),
                              request_id=f'request-{i}', model='synthetic-model', kind='generate',
                              state='settled' if settled else 'reserved', day=day,
                              budget_delta=(-90 if known else 0) if settled else 100,
                              input_tokens=5 if settled else 0, output_tokens=5 if settled else 0,
                              usage_known=known, outcome='ASNGN_ERR_CANCELLED' if settled else 'ASNGN_OK')
                data = json.dumps(record, separators=(',', ':')).encode()
                prefix = f'// asngn-wal-v2 {len(data)} {hashlib.sha256(data).hexdigest()}'.encode()
                target.write(prefix + b' ' + hashlib.sha256(prefix).hexdigest().encode() + b'\n' + data + b'\n')
    return ((count + 1) // 2) * 10 + (count // 2) * 100


def digest(path):
    with path.open('rb') as source:
        return hashlib.file_digest(source, 'sha256').hexdigest()


def measure(program, root):
    """wait4 reports this child's peak; it is not an accumulated prior maximum."""
    started = time.monotonic()
    with tempfile.TemporaryFile() as output:
        process = subprocess.Popen([str(program), str(root)], stdout=output, stderr=subprocess.STDOUT)
        while True:
            pid, status, usage = os.wait4(process.pid, os.WNOHANG)
            if pid:
                process.returncode = os.waitstatus_to_exitcode(status)
                break
            if time.monotonic() - started >= 60:
                process.kill()
                _, status, _ = os.wait4(process.pid, 0)
                process.returncode = os.waitstatus_to_exitcode(status)
                raise RuntimeError('probe deadline exceeded')
            time.sleep(.005)
        output.seek(0)
        text = output.read(4096).decode()
        if process.returncode:
            raise RuntimeError(f'probe failed ({process.returncode}): {text}')
    return json.loads(text), time.monotonic() - started, usage.ru_maxrss


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--before', type=Path, required=True)
    parser.add_argument('--after', type=Path, required=True)
    parser.add_argument('--report', type=Path, required=True)
    parser.add_argument('--operations', type=int, default=10000)
    parser.add_argument('--repeats', type=int, default=5)
    args = parser.parse_args()
    if sys.platform != 'linux':
        parser.error('Linux wait4 RSS units are required for this component measurement')
    if not 1 <= args.operations <= 100000 or not 1 <= args.repeats <= 100:
        parser.error('operations must be 1..100000 and repeats 1..100')
    report = dict(schema=1, operations=args.operations, repeats=args.repeats,
                  clock='monotonic wall including process startup',
                  memory='Linux wait4 process max RSS KiB; allocator/sanitizers affect results',
                  environment=dict(platform=os.uname().sysname, machine=os.uname().machine,
                                   asan_options=os.getenv('ASAN_OPTIONS')), runs=[])
    programs = {key: getattr(args, key).resolve() for key in ('before', 'after')}
    report['executables'] = {key: dict(path=str(path), sha256=digest(path)) for key, path in programs.items()}
    with tempfile.TemporaryDirectory(prefix='asterism-replay-benchmark-') as td:
        root = Path(td)
        journal = root / 'operations.xcdn'
        expected = fixture(journal, args.operations)
        original = digest(journal)
        report['fixture'] = dict(bytes=journal.stat().st_size, sha256=original, expected_daily_spent=expected)
        for repeat in range(args.repeats):
            # Alternate order; both readers see the same warm local fixture.
            for label in ('before', 'after') if repeat % 2 == 0 else ('after', 'before'):
                outcome, elapsed, rss = measure(programs[label], root)
                if outcome != dict(outcome='ASNGN_OK', daily_spent=expected) or digest(journal) != original:
                    raise RuntimeError('replay changed the journal or produced different accounting')
                report['runs'].append(dict(label=label, repeat=repeat, wall_seconds=elapsed,
                                           max_rss_kib=rss, result=outcome))
    report['medians'] = {label: {metric: statistics.median(r[metric] for r in report['runs'] if r['label'] == label)
                                 for metric in ('wall_seconds', 'max_rss_kib')} for label in programs}
    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.report.write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report['medians'], indent=2))


if __name__ == '__main__':
    main()
