# asngn telemetry events

One event shape everywhere: an xCDN value tagged `#asngn_event`
with `at` (RFC 3339 UTC wall clock, display only), `kind`, optional
`span`/`parent` (UUIDs forming the per-turn span tree), optional
`session` and `turn`, and a kind-specific `data` record. Durations are
measured on the monotonic clock. Consumers must tolerate unknown kinds
and unknown `data` fields (forward compatibility).

| kind          | data fields                                            |
|---------------|--------------------------------------------------------|
| `turn_start`  | `bytes` — size of the gated user message               |
| `classify`    | `class`, `detail`, `mode`, `task`, `tools`, `repo_files`, `escalated`, `unreliable`, `eval` (success rate or null), `source` ("heuristic" \| "model" \| "hybrid") |
| `route`       | `class`, `detail`, `mode`, `task`, `tier`; or `escalated: true` when the judge ladder moves the generator up a tier; or `start: "up"/"down"` when the evidence-gated initial tier moves |
| `cache_probe` | `outcome` ("hit" \| "adapt" \| "miss"), `cos`          |
| `model_call`  | `model` (pool id), `task` (classify \| decide \| draft \| answer \| compress \| adapt \| judge), `tokens_in`, `tokens_out`, `ms`, `tps` |
| `tool_call`   | `tool`, `command`, `ok`, `ms`; or `cached: true` for a tool-result-cache hit |
| `step`        | `action` (call \| recall \| open \| think \| clarify \| answer), `why` — the model's declared rationale (redacted, flattened, truncated) |
| `recall`      | (empty) — the recall step ran                          |
| `fold`        | `mode` ("compressor" \| "extractive")                  |
| `digest`      | `label` (tool.command or "recall"), `bytes` (full size)|
| `judge`       | `score` (0–10), `tokens`                               |
| `confirm`     | `confirm_id` (UUID), `tool`, `command`, `destructive`, `read_only`, `args` (truncated) — answer via `asngn_confirm` |
| `guard`       | `guard` — one of `stall`, `identical_call`, `oscillation`, `step_budget`, `think_limit`, `recall_limit`, `futile_steps`, `tool_cap`, `working_trim`, `budget_pressure`, `outcome_gate`, `response_protocol` |
| `answer`      | `tokens`, `capped`                                     |
| `turn_end`    | (empty), or `cancelled: true`                          |
| `error`       | `message`                                              |

Sinks: an in-memory ring of `telemetry.ring` events (always on;
the TUI and `asngn_set_event_sink` read it) and, with `telemetry.path`
set, an append stream `telemetry/telemetry.xcdn` flushed once per turn
with size-based rotation. A telemetry failure never fails the turn that
emitted it. The per-turn economic summary is the ledger —
telemetry holds the *how*, the ledger holds the *bill*.
