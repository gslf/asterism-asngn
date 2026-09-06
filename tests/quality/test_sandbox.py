"""Sandbox startup cannot be mistaken for a completed verifier command."""
import os
from pathlib import Path
import shutil
import tempfile
import unittest
from unittest.mock import patch

from checks import Check
from command import Result
from sandbox import STATUS_LIMIT, isolated, status_error

START = b'{"child-pid":123,"pid-namespace":4026531836}\n'
DONE = START + b'{"exit-code":0}\n'


class SandboxStatusTests(unittest.TestCase):
    def test_only_matching_terminal_exit_establishes_execution(self):
        for code in (0, 1, 137, 255):
            self.assertIsNone(status_error(START + f'{{"exit-code":{code}}}\n'.encode(), code))
        for data in (b'', START):
            self.assertEqual(status_error(data, 1), 'sandbox_status_incomplete')

    def test_malformed_and_ambiguous_status_fails_closed(self):
        for data in (b'\xff', b'[]\n', b'{"exit-code":0}\n',
                     b'{"child-pid":true}\n', b'{"child-pid":0}\n',
                     b'{"child-pid":1,"child-pid":2}\n',
                     START + b'{"exit-code":true}\n', START + b'{"exit-code":256}\n',
                     START + b'{"exit-code":0,"extra":0}\n',
                     START + b'{"exit-code":0,"exit-code":0}\n', DONE + b'{}\n',
                     START + b'{"exit-code":1}\n'):
            with self.subTest(data=data):
                self.assertEqual(status_error(data, 0), 'sandbox_status_invalid')
        self.assertEqual(status_error(b'x' * (STATUS_LIMIT + 1), 0), 'sandbox_status_limit')

    @unittest.skipUnless(os.name == 'posix', 'descriptor transport requires POSIX')
    def test_status_channel_is_separate_from_test_output_and_preserves_errors(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            for data, code, error, expected in (
                (DONE, 0, None, 'passed'),
                (START + b'{"exit-code":7}\n', 7, None, 'failed'),
                (START, 0, None, 'infrastructure_error'),
                (DONE, 0, 'deadline', 'infrastructure_error'),
            ):
                def monitor(argv, cwd, *, pass_fds):
                    fd, = pass_fds
                    self.assertEqual(argv[argv.index('--json-status-fd') + 1], str(fd))
                    self.assertIn('--unshare-all', argv)
                    os.write(fd, data)
                    # Even plausible status on stdout has no authority.
                    return Result(argv, code, DONE.decode(), error)
                with patch('sandbox.shutil.which', return_value='/usr/bin/bwrap'), \
                        patch('sandbox.run', side_effect=monitor):
                    result = isolated(['true'], root)
                receipt = Check(['true']).receipt(result)
                self.assertEqual(receipt['status'], expected)
                self.assertEqual(receipt['exit_code'], code)
                self.assertEqual(receipt['output'], DONE.decode())
                if error:
                    self.assertEqual(receipt['reason'], error)

    def test_unavailable_sandbox_and_status_io_are_infrastructure_failures(self):
        with patch('sandbox.shutil.which', return_value=None):
            self.assertEqual(isolated(['true'], Path('.')).error, 'sandbox_unavailable')
        with tempfile.TemporaryDirectory() as td:
            with patch('sandbox.shutil.which', return_value='/usr/bin/bwrap'), \
                    patch('sandbox.tempfile.TemporaryFile', side_effect=OSError('disk full')):
                self.assertEqual(isolated(['true'], Path(td)).error, 'sandbox_status_io')

    @unittest.skipUnless(shutil.which('bwrap'), 'Linux bubblewrap unavailable')
    def test_real_setup_or_exec_failure_has_no_terminal_receipt(self):
        with tempfile.TemporaryDirectory() as td:
            result = isolated(['/nonexistent/asterism-verifier'], Path(td))
        self.assertNotEqual(result.returncode, 0)
        receipt = Check(['/nonexistent/asterism-verifier']).receipt(result)
        self.assertEqual(receipt['status'], 'infrastructure_error', receipt)
        self.assertEqual(receipt['reason'], 'sandbox_status_incomplete', receipt)


if __name__ == '__main__':
    unittest.main()
