# Protected coding smoke evaluation

This is a two-task smoke suite, not a repository coding benchmark or a leaderboard.
Contract tests use deterministic local fixtures; the runner below requires real
configured models and tools. Linux bubblewrap and the task toolchains are required.

```sh
python3 run_quality.py --asngn /absolute/build/asngn \
  --engine-root /absolute/evaluation-inputs --profile model-quant-backend-hardware \
  --repeats 3 --report /absolute/results/report.json
```

Each trial gets a fresh engine directory. Only `config.xcdn` is copied and an
optional `models/` directory is linked for weights; use absolute paths for other
configuration inputs. Memory, sessions, response caches and calibration are not
imported. This runner never writes routing calibration. Reports are inputs to a
separate, explicit offline promotion decision. Both fixtures and their acceptance
checks are public, so reports always identify the split as `dev`. Repetitions and
fresh directories do not turn public tasks into an unseen holdout.

The verifier reconstructs a fresh fixture, audits the patch, adds evaluator-only checks
and executes outside the agent workspace. Original tests/build files are read-only.
Additional tests remain in the patch artifact but cannot influence acceptance.
Only implementation changes and bounded test-source additions are accepted by
these fixtures. General repository benchmarks need task-specific patch policies.
The C header is a protected public contract. Binary patches are not admitted.

Schema 3 distinguishes successful commands from completed tests. The Python
runner requires two original and three additional tests; the C probe completes
four explicit conditions after the original CTest suite. Each probe emits one
structured record bound to its invocation. Zero exit without that record is
`inconclusive`; empty, skipped, incomplete or malformed results cannot pass.
Test failures and infrastructure failures remain distinct, and dependent checks
stop after a failure. This prevents ordinary early exits and disabled collection
from certifying a repair. Arbitrary hostile code inside a test process can inspect
or alter its interpreter and try to forge test output; these probes do not prove
resistance to every such attack, nor do public cases establish generalization.
Reports identify the evaluator source and admitted patch by SHA-256; each check
retains its actual exit status, completion counts, rejection reason and output.

The sandbox monitor writes lifecycle JSON to a private, unlinked descriptor that
is closed in the payload before exec. Acceptance requires its terminal exit record
to match the observed process status. Setup/exec failure, incomplete or malformed
monitor state is `infrastructure_error`, even when the process exits zero or
stdout resembles a receipt. A child PID alone does not establish execution.
Deadlines and output limits retain their original error. This uses bubblewrap's
[`--json-status-fd` contract](https://github.com/containers/bubblewrap/blob/v0.11.2/bubblewrap.c);
an installation without that option cannot produce a successful verification.
The monitor proves execution lifecycle, not the correctness of a test assertion.

POSIX commands, agent output and patch capture retain at most 4 MiB each. A single
deadline covers execution and pipe collection; limits, missing executables and
truncated output fail closed. The verifier's PID namespace and process-group
cleanup stop descendants; process groups alone cannot contain children that
create another session outside the verifier. CPU/RAM/disk resource isolation for
arbitrary hostile workloads requires an external evaluation environment. A missing
or oversized telemetry file yields unknown counters, not a partial measurement.

Reports retain executable checks, patches, per-trial outcomes, timeout, p50/p95,
Wilson intervals and sampled Linux process-tree RSS. Shared pages may be counted
more than once; GPU memory is unavailable. The two tasks are correlated across
repetitions, so the interval is not evidence of population-wide performance.
False-success claims remain unavailable until the agent exposes a typed,
independently verifiable task-success state. Exit zero is only a turn outcome.
