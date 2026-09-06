"""Evaluator-owned check identities and strict completion receipt validation."""
from dataclasses import dataclass, field
import json
import uuid

PREFIX = 'ASTERISM_CHECK '
COUNTS = ('collected', 'executed', 'skipped', 'failures', 'errors')


@dataclass
class Check:
    command: list[str]
    expected: int | None = None
    identity: str = field(default_factory=lambda: uuid.uuid4().hex)

    def receipt(self, result):
        receipt = {
            'action_id': self.identity, 'command': self.command,
            'kind': 'test' if self.expected is not None else 'command',
            'expected': self.expected, 'counts': None,
            'exit_code': result.returncode, 'output': result.stdout,
            'execution_error': result.error, 'status': 'inconclusive',
            'reason': None,
        }
        counts, reason = completion(result.stdout, self.identity) if self.expected is not None else (None, None)
        receipt['counts'], receipt['reason'] = counts, reason
        if result.error:
            receipt['status'] = 'infrastructure_error'
            receipt['reason'] = result.error
        elif result.returncode != 0:
            receipt['status'] = 'failed'
            receipt['reason'] = 'nonzero_exit'
        elif self.expected is None:
            receipt['status'] = 'passed'
        elif counts is not None:
            if counts['failures'] or counts['errors']:
                receipt['status'] = 'failed'
                receipt['reason'] = 'test_failures'
            elif not counts['collected'] or not counts['executed']:
                receipt['status'] = 'not_run'
                receipt['reason'] = 'empty_suite'
            elif (counts['collected'] == counts['executed'] == self.expected and
                  counts['skipped'] == 0):
                receipt['status'] = 'passed'
            else:
                receipt['reason'] = 'incomplete_suite'
        return receipt


def completion(output, identity):
    records = [line[len(PREFIX):] for line in output.splitlines() if line.startswith(PREFIX)]
    if len(records) != 1:
        return None, 'missing_completion' if not records else 'duplicate_completion'
    try:
        data = json.loads(records[0], object_pairs_hook=unique_fields)
    except (ValueError, TypeError, RecursionError):
        return None, 'invalid_completion'
    fields = {'schema', 'action_id', *COUNTS}
    if (not isinstance(data, dict) or set(data) != fields or
            type(data['schema']) is not int or data['schema'] != 1 or
            data['action_id'] != identity or
            any(type(data[key]) is not int or data[key] < 0 for key in COUNTS)):
        return None, 'invalid_completion'
    return {key: data[key] for key in COUNTS}, None


def unique_fields(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError('duplicate receipt field')
        result[key] = value
    return result
