"""Completion records are evidence only under the expected invocation identity."""
import json
import unittest

from checks import Check, PREFIX
from command import Result


class CheckTests(unittest.TestCase):
    def test_missing_malformed_or_misattributed_records_cannot_pass(self):
        check = Check(['example'], 2)
        record = dict(schema=1, action_id=check.identity, collected=2, executed=2,
                      skipped=0, failures=0, errors=0)
        valid = PREFIX + json.dumps(record)
        variants = ['', '{}', valid + '\n' + valid,
                    valid.replace(check.identity, 'wrong'),
                    valid.replace('"executed": 2', '"executed": true'),
                    valid.replace('"executed": 2', '"executed": 2, "executed": 2'),
                    valid.replace('"executed": 2', '"executed": 1'),
                    valid.replace('"skipped": 0', '"skipped": 1')]
        for text in variants:
            with self.subTest(record=text):
                self.assertEqual(check.receipt(Result([], 0, text))['status'], 'inconclusive')
        self.assertEqual(check.receipt(Result([], 0, valid))['status'], 'passed')
        self.assertEqual(check.receipt(Result([], 1, valid))['status'], 'failed')
        self.assertEqual(check.receipt(Result([], 0, valid, 'output_limit'))['status'],
                         'infrastructure_error')

    def test_empty_suite_and_command_are_distinct(self):
        check = Check(['example'], 2)
        text = PREFIX + json.dumps(dict(schema=1, action_id=check.identity,
                                       collected=0, executed=0, skipped=0, failures=0, errors=0))
        self.assertEqual(check.receipt(Result([], 0, text))['status'], 'not_run')
        self.assertEqual(Check(['example']).receipt(Result([], 0))['kind'], 'command')
