# ⁂ asngn telemetry events

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
| `model_call`  | `model` (pool id), `task`, `tokens_in`, `tokens_out`, `ms` (runtime attempt), `outcome`, `usage_known`, `finish_reason`, `runtime_dispatch_attempted: true`; span links adapter request and result |
| `model_not_run` | Same outcome shape, with `runtime_dispatch_attempted: false` and known zero inference usage; context/deadline rejected the request before runtime dispatch |
| `tool_call`   | `tool`, `command`, `ok`, `ms`; or `cached: true` for a tool-result-cache hit |
| `step`        | `action` (call \| discover \| recall \| open \| think \| clarify \| answer), `why` — the model's declared rationale (redacted, flattened, truncated) |
| `recall`      | (empty) — the recall step ran                          |
| `fold`        | `mode` ("compressor" \| "extractive")                  |
| `evidence_selection` | `schema`, `policy`, `source` (object hash), `source_bytes`, `selected_bytes`, `diagnostic_markers`, `excerpt_budget_bytes`, `compressor_used`, `spans` (`start`, `end`, `reason`) |
| `context_selection` | `schema`, `policy`, `scope`, `model`, `phase`, `observed_snapshot`, `memory_owner`, `count_basis`, budgets, `zone_tokens` (attribution quality unknown), prompt hashes, fragment decisions and `items_omitted`; no source text |
| `request_context` | `scope: model_adapter_input`, `model`, `task`, `phase`, `admission`, context budgets (null if unmeasured), sampling/reasoning settings, output-contract fingerprints, `input_contract_sha256`, message/tool counts, at most 128 item fingerprints and explicit omissions; no original payloads |
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
Generation spans correlate with the durable `request_id`, while the accounting
operation keeps its own identity. See [record and replay semantics](operations.md).
