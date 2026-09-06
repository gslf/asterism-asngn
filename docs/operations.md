# Durable inference consumption

The shared backend reserves budget before invoking inference and settles it from
that call's usage, including failure and cancellation. An unknown result retains
the reservation; neither a conversation rollback nor a missing trace refunds it.
One projection now serves live admission, restart recovery and public inspection,
with separate lifetime and reservation-day totals for the entire engine store.
It includes generation and embedding through the shared manager, including Asper.
Complete session attribution, monetary pricing, loader costs and external
reconciliation remain separate work. Missing session identity is never guessed
from whichever foreground task happens to be running.

## Inspect consumption

Use `asngn_get_consumption` in C, `engine_consumption` with `{}` over MCP, or
`await client.consumption()` in either SDK. No session is opened, no inference
is invoked and inspection never repairs or rewrites the journal. The C getter
returns a bounded atomic snapshot without scanning the history on each read.
An uncertain journal returns `ASNGN_ERR_IO` and clears the output; a stale
successful-looking projection is not substituted.

| Counter in `lifetime` and `today` | Meaning |
|---|---|
| `calls` | Durable reservations, whether settled or not |
| `unsettled_calls`, `unsettled_tokens` | Reservations without a terminal receipt; they may be live or interrupted |
| `unknown_calls`, `unknown_tokens` | Settled calls whose final usage is unknown; the original reservation remains charged |
| `known_input_tokens`, `known_output_tokens` | Usage from settlements explicitly marked known, including failures and cancellations |
| `failed_calls` | Settled outcomes other than success or cancellation |
| `cancelled_calls` | Settled backend cancellations, separate from failed calls |
| `charged_tokens` | Known input/output plus unsettled and unknown reservations |

Zero charged tokens do not establish that every call had known usage: inspect
the call counters too. `charged_tokens` is a budget charge, not an exact physical
or monetary measurement. Backend success does not mean that later application
validation or the task succeeded. A failed turn can have nonzero consumption and
zero committed conversation tokens.

MCP schema 1 declares `scope: "engine_store"`, `unit: "tokens"` and
`time_basis: "reservation_utc_day"`. Token/call counters use canonical decimal
strings so JavaScript cannot round integers above 2^53. Python converts them to
`int`; JavaScript uses `bigint`. The SDKs validate scalar bounds, counter sums and
daily/lifetime consistency. The raw `call_tool`/`callTool` result retains strings.
The TUI stats pane labels committed conversation totals separately from known,
unsettled, unknown and charged engine tokens. Existing session `spent_tokens`
and the `session_tokens` soft policy remain committed-turn projections; they
must not be used as a complete inference bill or per-session admission limit.

`today` uses the greatest current or recorded reservation UTC day. Reading after
midnight updates this view without requiring another inference. Settling an old
reservation changes its lifetime accounting, not the new day's allowance.
New reservations never move behind an already observed day, so wall-clock
rollback cannot reuse an earlier allowance. Reads do not persist clock changes;
restart reconstructs the high-water mark from reservations and the current clock.
Lifetime limits and arithmetic remain checked even when daily counters reset.

## Durable records

Generation request spans travel through asmodel ABI 8 to both durable records as
`request_id`. The accounting `id` is a separate, runtime-generated UUID. Repeated
host correlation labels cannot merge operations or trigger replay. Engine context
rejection creates no inference reservation. Manager/load rejection may likewise
occur before the backend reaches reservation. An empty `request_id` means that
the caller supplied no trace identity, as with current direct Asper/embedding
calls. These operations are still accounted for individually.

`operations.xcdn` contains schema-2 JSON payloads in version-2 checksummed WAL
frames. Each record includes model, operation kind, request ID, day, usage,
outcome and the budget delta. Strict decoding rejects duplicate/unknown fields,
embedded NULs, invalid counts and mismatched settlement identities. A backend
outcome is not the task outcome: application validation can still reject output.
Schema-1 operation stores are not opened or silently upgraded by this version.
Use a fresh development store; no existing store was converted by this change.

Replay validates one frame at a time, capped at 4 KiB, with at most 262,144 records
and the shared 256 MiB WAL limit. A growing identity table retains reservation and
settlement state, rather than the complete text and parsed history. Accounting is
published only after the entire replay succeeds. Complete invalid records leave
the file intact, even if an incomplete tail follows. Incomplete-tail repair still
requires successful prefix validation and sync. Other WAL consumers now decode
complete frames separately; xCDN's general duplicate-key semantics are unchanged.

Reservation identity strings are copied before dispatch, so caller mutation
cannot silently reassign the settlement. Reservation/settlement arithmetic is
checked before signed overflow. An uncertain
write blocks further inference admission and settlement appends until recovery;
already consumed inference remains represented by its outstanding reservation.
This does not certify power-loss behavior or bound a native model loader.

Seven counter tests cover outcomes without commits, zero-usage uncertainty,
midnight and clock rollback, lifetime overflow, copied identities, uncertain sync
and four concurrent lanes. Actual worker failure/cancellation retains charges
after conversational rollback. Both SDKs inspect a checksummed history across
process restarts without changing its bytes, including counters above 2^53.
These are contract checks, not real-model cost measurements.

## Component measurement

The [raw local run](benchmarks/operation-replay.json) compares an intermediate
schema-2 implementation that accumulated an xCDN document with the streaming
decoder. Both are GCC `-O3` builds with ASan/UBSan and leak detection disabled;
executable SHA-256 values identify those exact artifacts. This is a synthetic
component comparison, not a model or release-performance benchmark.

Five alternating runs read the same warm Linux fixture of 10,000 interleaved
reservations and settlements. Every result was 550,000 charged tokens, and the
journal hash remained unchanged. Median maximum process RSS was 161,176 versus
122,156 KiB; median wall time including startup was 1.087 versus 0.776 seconds.
Sanitizer quarantine affects these measurements; other allocators, ordinary
builds, larger histories and cold storage require their own measurements.

Build `asngn_operation_replay_probe` with tests enabled, then compare two probe
executables that accept schema 2:

```sh
python3 tests/benchmark_operations.py --before /absolute/previous-probe \
  --after /absolute/current-probe --operations 10000 --repeats 5 \
  --report /absolute/results/replay.json
```

The script creates and deletes only its synthetic store. Linux `wait4` supplies
per-process maximum RSS; a 5 ms poll interval bounds completion timing precision.
These probes are development tools, not maintenance commands for user stores.
