# Blocking coding quality suite

This suite runs the real model on disposable Git repositories. Every scenario
starts with a failing build/test, asks the agent to diagnose and repair the
repository with tools, and accepts the run only when the resulting patch is
non-empty, cleanly applicable and all declared checks pass.

```sh
python3 tests/quality/run_quality.py \
  --asngn ./build/asngn --engine-root ./demo \
  --report ./build/quality-report.json
```

The blocking metrics are task success, post-change build/tests, patch
applicability, valid tool use, useless attempts/guard trips, regressions,
latency and peak resident memory. QpT is diagnostic only and never contributes
to pass/fail. A task without an external result check has no quality score; it
is never assigned an automatic `0.5`.

Every task must succeed. Each patch must pass `git apply --check` against its
baseline, use at least one valid tool call, stay below twelve tool calls and
three guard trips, and pass the complete scenario-specific verification suite.
The scheduled real-model workflow runs this harness as a blocking step and
uploads its JSON report and patches as evidence.
