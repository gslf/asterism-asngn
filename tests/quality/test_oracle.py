"""Regression tests for evaluator-owned acceptance fixtures."""
import tempfile
import unittest
import os
import shutil
from pathlib import Path
from run_quality import TASKS, init_repo, read_bounded, run, verify_patch
from command import OUTPUT_LIMIT


PYTHON_FIX = '''from copy import deepcopy

def merge_defaults(defaults, override):
    result = deepcopy(defaults)
    for key, value in override.items():
        if isinstance(value, dict) and isinstance(result.get(key), dict):
            result[key] = merge_defaults(result[key], value)
        else:
            result[key] = deepcopy(value)
    return result
'''


@unittest.skipUnless(os.name == 'posix', 'the evaluator requires POSIX')
class OracleTests(unittest.TestCase):
    def check_patch(self, edit, *, task=TASKS[1], sandbox=False):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            init_repo(root, task)
            edit(root)
            run(['git', 'add', '--intent-to-add', '.'], root)
            patch = root / 'candidate.patch'
            patch.write_text(run(['git', 'diff', '--binary', 'HEAD'], root).stdout)
            return verify_patch(task, patch, sandbox=sandbox)

    def test_replacing_acceptance_test_is_rejected(self):
        def edit(root):
            (root / 'test_config_merge.py').write_text('pass\n')
        audit, applied, checks = self.check_patch(edit)
        self.assertTrue(applied)
        self.assertFalse(audit)
        self.assertEqual(checks, [])

    def test_nonempty_patch_leaving_bug_fails(self):
        def edit(root):
            with (root / 'config_merge.py').open('a') as f:
                f.write('\n# No repair.\n')
        audit, applied, checks = self.check_patch(edit)
        self.assertTrue(audit and applied)
        self.assertTrue(any(c['exit_code'] != 0 for c in checks))

    def test_added_tests_cannot_disable_original_checks(self):
        def edit(root):
            (root / 'test_trivial.py').write_text('import unittest\nunittest.TestCase.run = lambda *a: None\n')
        audit, applied, checks = self.check_patch(edit)
        self.assertTrue(audit and applied)
        self.assertTrue(any(c['exit_code'] != 0 for c in checks))

    def test_arbitrary_added_code_is_rejected(self):
        def edit(root):
            (root / 'sitecustomize.py').write_text('raise SystemExit(0)\n')
        audit, applied, checks = self.check_patch(edit)
        self.assertTrue(applied)
        self.assertFalse(audit)
        self.assertEqual(checks, [])

    def test_general_recursive_fix_passes(self):
        def edit(root):
            (root / 'config_merge.py').write_text(PYTHON_FIX)
        audit, applied, checks = self.check_patch(edit)
        self.assertTrue(audit and applied)
        self.assertEqual([c['status'] for c in checks], ['passed', 'passed'])
        self.assertEqual([c['counts']['executed'] for c in checks], [2, 3])

    def test_successful_exit_without_checks_is_inconclusive(self):
        for exit_code in ('import os\nos._exit(0)\n', 'raise SystemExit(0)\n'):
            with self.subTest(code=exit_code):
                _, applied, checks = self.check_patch(
                    lambda root: (root / 'config_merge.py').write_text(exit_code))
                self.assertTrue(applied)
                self.assertEqual(checks[0]['exit_code'], 0)
                self.assertEqual(checks[0]['status'], 'inconclusive')

    def test_disabled_and_skipped_tests_cannot_pass(self):
        for alteration in (
            'import unittest\nunittest.TestCase.run = lambda *a: None\n',
            'import unittest\nunittest.TestCase.__unittest_skip__ = True\n',
        ):
            with self.subTest(alteration=alteration):
                _, _, checks = self.check_patch(
                    lambda root: (root / 'config_merge.py').write_text(alteration + PYTHON_FIX))
                self.assertEqual(checks[0]['exit_code'], 0)
                self.assertNotEqual(checks[0]['status'], 'passed')

    def test_c_header_is_a_protected_contract(self):
        audit, applied, checks = self.check_patch(
            lambda root: (root / 'range.h').write_text('#define NDEBUG\n' + TASKS[0]['files']['range.h']),
            task=TASKS[0])
        self.assertTrue(applied)
        self.assertFalse(audit)
        self.assertEqual(checks, [])

    def test_c_repair_and_early_exit(self):
        fixed = TASKS[0]['files']['range.c'].replace('i <= count', 'i < count')
        exited = '#include <stdlib.h>\nint range_sum(const int *v, int n) { exit(0); }\n'
        for source, expected in [(fixed, 'passed'), (exited, 'inconclusive')]:
            with self.subTest(expected=expected):
                audit, applied, checks = self.check_patch(
                    lambda root: (root / 'range.c').write_text(source), task=TASKS[0])
                self.assertTrue(audit and applied)
                self.assertEqual(len(checks), 5)
                self.assertEqual(checks[-1]['status'], expected)
                self.assertEqual(checks[-1]['exit_code'], 0)

    @unittest.skipUnless(shutil.which('bwrap'), 'Linux bubblewrap unavailable')
    def test_production_sandbox_accepts_repairs_and_rejects_early_exits(self):
        cases = [
            (TASKS[1], 'config_merge.py', PYTHON_FIX, 'passed'),
            (TASKS[1], 'config_merge.py', 'import os\nos._exit(0)\n', 'inconclusive'),
            (TASKS[0], 'range.c', TASKS[0]['files']['range.c'].replace('i <= count', 'i < count'), 'passed'),
        ]
        for task, name, source, expected in cases:
            with self.subTest(task=task['name'], expected=expected):
                audit, applied, checks = self.check_patch(
                    lambda root: (root / name).write_text(source), task=task, sandbox=True)
                self.assertTrue(audit and applied)
                self.assertEqual(checks[-1]['status'], expected, checks)

    def test_compilation_failure_does_not_run_dependent_checks(self):
        _, _, checks = self.check_patch(
            lambda root: (root / 'range.c').write_text('invalid C\n'), task=TASKS[0])
        self.assertEqual(len(checks), 2)
        self.assertEqual(checks[-1]['status'], 'failed')

    def test_binary_and_oversized_patches_are_rejected_before_application(self):
        for data in (b'diff --git a/range.c b/range.c\nGIT binary patch\nliteral 100000000\n',
                     b'x' * (OUTPUT_LIMIT + 1)):
            with tempfile.TemporaryDirectory() as td:
                path = Path(td) / 'candidate.patch'
                path.write_bytes(data)
                self.assertEqual(verify_patch(TASKS[0], path, sandbox=False), (False, False, []))

    def test_input_reading_rejects_special_files_and_final_symlinks(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            (root / 'ordinary').write_bytes(b'observed')
            (root / 'link').symlink_to(root / 'ordinary')
            os.mkfifo(root / 'pipe')
            for name in ('link', 'pipe', 'missing'):
                self.assertIsNone(read_bounded(root / name))
            self.assertEqual(read_bounded(root / 'ordinary'), b'observed')


if __name__ == '__main__':
    unittest.main()
