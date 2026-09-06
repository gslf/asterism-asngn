"""Copied into the protected fixture after patch application, then run with -I."""
import json
from pathlib import Path
import sys
import unittest


def main():
    module, identity = sys.argv[1:]
    sys.path.insert(0, str(Path(__file__).parent))
    suite = unittest.defaultTestLoader.loadTestsFromName(module)
    collected = suite.countTestCases()
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    record = {
        'schema': 1, 'action_id': identity, 'collected': collected,
        'executed': result.testsRun,
        'skipped': len(result.skipped) + len(result.expectedFailures),
        'failures': len(result.failures),
        'errors': len(result.errors) + len(result.unexpectedSuccesses),
    }
    # A normal exit without this record never certifies completion.
    print('ASTERISM_CHECK ' + json.dumps(record), flush=True)
    return 0 if result.wasSuccessful() else 1


if __name__ == '__main__':
    raise SystemExit(main())
