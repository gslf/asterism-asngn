"""Regression tests for evaluator-owned acceptance fixtures."""
import tempfile
import unittest
from pathlib import Path
from run_quality import TASKS, init_repo, run, verify_patch


class OracleTests(unittest.TestCase):
    def check_patch(self, edit):
        task = TASKS[1]
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            init_repo(root, task)
            edit(root)
            run(['git', 'add', '--intent-to-add', '.'], root)
            patch = root / 'candidate.patch'
            patch.write_text(run(['git', 'diff', '--binary', 'HEAD'], root).stdout)
            return verify_patch(task, patch, sandbox=False)

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
            (root / 'config_merge.py').write_text('''from copy import deepcopy

def merge_defaults(defaults, override):
    result = deepcopy(defaults)
    for key, value in override.items():
        if isinstance(value, dict) and isinstance(result.get(key), dict):
            result[key] = merge_defaults(result[key], value)
        else:
            result[key] = deepcopy(value)
    return result
''')
        audit, applied, checks = self.check_patch(edit)
        self.assertTrue(audit and applied)
        self.assertTrue(checks)
        self.assertTrue(all(c['exit_code'] == 0 for c in checks))


if __name__ == '__main__':
    unittest.main()
