"""Real subprocess regressions for output bounds, deadlines and orphaned pipes."""
import os
from pathlib import Path
import signal
import sys
import tempfile
import time
import unittest

from command import run


@unittest.skipUnless(os.name == 'posix', 'the evaluator requires POSIX')
class CommandTests(unittest.TestCase):
    def python(self, source, **kwargs):
        return run([sys.executable, '-I', '-B', '-c', source], Path.cwd(), **kwargs)

    def test_output_limit_keeps_a_bounded_prefix_and_cannot_pass(self):
        result = self.python('import os\nwhile True: os.write(1, b"x" * 65536)', output_limit=1024)
        self.assertEqual(result.stdout, 'x' * 1024)
        self.assertEqual(result.error, 'output_limit')
        self.assertFalse(result.ok)

    def test_exit_codes_and_missing_executable_are_preserved(self):
        result = self.python('print("observed"); raise SystemExit(7)')
        self.assertEqual((result.returncode, result.stdout, result.error), (7, 'observed\n', None))
        result = run(['/this/executable/does/not/exist'], Path.cwd())
        self.assertIsNone(result.returncode)
        self.assertEqual(result.error, 'startup_error')

    def test_explicit_status_descriptor_reaches_only_the_requested_process(self):
        with tempfile.TemporaryFile() as status:
            fd = status.fileno()
            result = self.python(f'import os; os.write({fd}, b"receipt")', pass_fds=(fd,))
            self.assertTrue(result.ok, result)
            status.seek(0)
            self.assertEqual(status.read(), b'receipt')
            result = self.python(f'import os; os.write({fd}, b"not inherited")')
            self.assertNotEqual(result.returncode, 0)
            status.seek(0)
            self.assertEqual(status.read(), b'receipt')

    def test_deadline_includes_process_with_closed_stdout(self):
        result = self.python('import os, time; os.close(1); os.close(2); time.sleep(60)', timeout=.15)
        self.assertEqual(result.error, 'deadline')
        self.assertEqual(result.returncode, -signal.SIGKILL)

    def test_exited_parent_cannot_leave_a_pipe_holding_child(self):
        started = time.monotonic()
        result = self.python('import subprocess, sys\n'
                             'subprocess.Popen([sys.executable, "-c", "import time; time.sleep(60)"])',
                             timeout=3)
        self.assertTrue(result.ok, result)
        self.assertLess(time.monotonic() - started, 2)

    def test_escaped_process_cannot_hold_the_reader_past_its_deadline(self):
        with tempfile.TemporaryDirectory() as td:
            path = Path(td) / 'child'
            source = ('import subprocess, sys\n'
                      'p = subprocess.Popen([sys.executable, "-c", "import time; time.sleep(60)"], '
                      'start_new_session=True)\n'
                      f'open({str(path)!r}, "w").write(str(p.pid))\n')
            try:
                result = self.python(source, timeout=.5)
                self.assertEqual(result.error, 'deadline')
                self.assertEqual(result.returncode, 0)
                self.assertFalse(result.ok)
            finally:
                if path.exists():
                    os.kill(int(path.read_text()), signal.SIGKILL)
