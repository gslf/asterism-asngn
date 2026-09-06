# Durable inference consumption

The shared backend reserves budget before invoking inference and settles it from
that call's usage, including failure and cancellation. An unknown result retains
the reservation; neither a conversation rollback nor a missing trace refunds it.
The current projection enforces the UTC-day budget. Session-lifetime totals,
monetary pricing, loader costs and external reconciliation remain separate work.

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

Reservation/settlement arithmetic is checked before signed overflow. An uncertain
write blocks further inference admission and settlement appends until recovery;
already consumed inference remains represented by its outstanding reservation.
This does not certify power-loss behavior or bound a native model loader.

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
