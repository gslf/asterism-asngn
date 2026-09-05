# Protected coding smoke evaluation

This is a two-task smoke suite, not a repository coding benchmark or a leaderboard.
Contract tests use deterministic local fixtures; the runner below requires real
configured models and tools. Linux bubblewrap and the task toolchains are required.

```sh
python3 run_quality.py --asngn /absolute/build/asngn \
  --engine-root /absolute/evaluation-inputs --profile model-quant-backend-hardware \
  --split holdout --repeats 3 --report /absolute/results/report.json
```

Each trial gets a fresh engine directory. Only `config.xcdn` is copied and an
optional `models/` directory is linked for weights; use absolute paths for other
configuration inputs. Memory, sessions, response caches and calibration are not
imported. This runner never writes routing calibration. Reports are inputs to a
separate, explicit offline promotion decision; holdout data must not be promoted.

The verifier reconstructs a fresh fixture, audits the patch, adds hidden checks
and executes outside the agent workspace. Original tests/build files are read-only.
Additional tests remain in the patch artifact but cannot influence acceptance.
Only implementation changes and bounded test-source additions are accepted by
these fixtures. General repository benchmarks need task-specific patch policies.

Reports retain executable checks, patches, per-trial outcomes, timeout, p50/p95,
Wilson intervals and sampled Linux process-tree RSS. Shared pages may be counted
more than once; GPU memory is unavailable. The two tasks are correlated across
repetitions, so the interval is not evidence of population-wide performance.
False-success claims remain unavailable until the agent exposes a typed,
independently verifiable task-success state. Exit zero is only a turn outcome.
