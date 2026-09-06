# asngn telemetry events

One event shape everywhere: an xCDN value tagged `#asngn_event`
with `at` (RFC 3339 UTC wall clock, display only), `kind`, optional
`span`/`parent` (UUIDs forming the per-turn span tree), optional
`session` and `turn`, and a kind-specific `data` record. Durations are
measured on the monotonic clock. Consumers must tolerate unknown kinds
and unknown `data` fields (forward compatibility).

| kind          | data fields                                            |
|---------------|--------------------------------------------------------|
| `turn_start`  | `bytes`, `mode`, `security_profile`                     |
| `classify`    | `class`, `detail`, `mode`, `task`, `tools`, `repo_files`, `escalated`, `unreliable`, `eval` (success rate or null), `source` ("heuristic" \| "model" \| "hybrid") |
| `route`       | `class`, `detail`, `mode`, `task`, `tier`; or `escalated: true` when the judge ladder moves the generator up a tier; or `start: "up"/"down"` when the evidence-gated initial tier moves |
| `cache_probe` | `outcome` ("hit" \| "adapt" \| "miss"), `cos`          |
| `model_call`  | `model` (pool id), `task` (classify \| decide \| draft \| answer \| compress \| adapt \| judge), `tokens_in`, `tokens_out`, `ms`, `tps` |
| `tool_call`   | `tool`, `command`, `ok`, `ms`; or `cached: true` for a tool-result-cache hit |
| `step`        | `action` (call \| discover \| recall \| open \| think \| clarify \| answer), `why` — the model's declared rationale (redacted, flattened, truncated) |
| `recall`      | (empty) — the recall step ran                          |
| `fold`        | `mode` ("compressor" \| "extractive")                  |
| `evidence_selection` | `schema`, `policy`, `source` (object hash), `source_bytes`, `selected_bytes`, `diagnostic_markers`, `excerpt_budget_bytes`, `compressor_used`, `spans` (`start`, `end`, `reason`) |
| `context_selection` | `schema`, `policy`, `scope`, `model`, `phase`, `observed_snapshot`, `memory_owner`, `count_basis`, budgets, `zone_tokens` (attribution quality unknown), prompt hashes, fragment decisions and `items_omitted`; no source text |
| `retrieval_scan` | `schema`, `policy`, `entries`, `files`, `excluded`, `ignored`, `chunks`, `complete`, `error`; completeness describes traversal, not semantic evidence coverage |
| `judge`       | `score` (0–10), `tokens`                               |
| `confirm`     | `confirm_id` (UUID), `tool`, `command`, `destructive`, `read_only`, `args` (truncated), `arguments_sha256`, `package_sha256`, `snapshot` — inspect full redacted arguments via `asngn_approval_get`, answer via `asngn_confirm` |
| `authorization` | `granted`, `profile` — a profile denied an action; the turn may continue and report the required grant |
| `guard`       | `guard` — one of `stall`, `identical_call`, `oscillation`, `step_budget`, `think_limit`, `recall_limit`, `futile_steps`, `tool_cap`, `working_trim`, `budget_pressure`, `outcome_gate`, `response_protocol` |
| `answer`      | `tokens`, `capped`                                     |
| `turn_end`    | `ok`, `cancelled`, `error` (stable `asngn_err` name, empty on success). Exactly one is emitted for every submitted turn, including phase failures. |
| `error`       | `message`                                              |

Sinks: an in-memory ring of at most `telemetry.ring` events and 8 MiB of payload (always on;
the TUI and `asngn_set_event_sink` read it) and, with `telemetry.path`
set, an append stream `telemetry/telemetry.xcdn` flushed each turn and before
a batch exceeds 256 KiB, with size-based rotation. Events larger than 64 KiB
are dropped. A telemetry failure never fails the turn that emitted it.
See [selection trace semantics](evidence-context.md). Consumption reservations
and settlements are durable operation records; conversation commits and
best-effort telemetry do not replace that accounting.
