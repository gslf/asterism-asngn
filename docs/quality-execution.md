# Quality execution plan

This tracker distinguishes implementation, contract tests and model evaluation.
It does not claim that the full research/product program is complete.
Baseline on 2026-09-05: asngn 8869910, asper 5295ced, asmodel 5ccdc3b,
astools efa6d22. Local source changes are included in the tested builds.

## Milestones

| Milestone | Work items | Implemented and tested | Remaining gate |
|---|---|---|---|
| 1. Trustworthy outcomes | VERIFY-01, EVAL-01, verifier part of TOOLS-01 | Typed receipts, action/snapshot binding, stale-proof rejection, test collection, independent protected oracle | Expand adapters, toolchain identity and protected repository task suite |
| 2. Reproducible foundation | RELEASE-01, TOKENS-01, USAGE-01 | Release manifest, ABI/header checks, standalone builds, explicit token uncertainty, durable operation reservations | Clean release reconstruction, published pins, calibrated remote tokenizer margins |
| 3. Safe state | WORKSPACE-01, STORAGE-01, ACTIONS-01, CONCURRENCY-01 | Descriptor-relative reads, bounded snapshots, expected edit hashes, writer lock, framed WAL/checksums, I/O fault tests | Incremental snapshots, durable approval recovery, standalone sibling locks, compaction/migration, cross-process workspace coordination |
| 4. Runtime contract | RUNTIME-01, PROTOCOL-01, PROVIDERS-01, EMBED-01 | One asmodel residency owner across lanes, intact cancellation/errors/partial output, per-request usage, elapsed queue time charged to deadline | Typed message/constraint IR, interruptible queue waits, native sequence batching, embedding pipeline identity/batch/deadline, real provider conformance |
| 5. Evidence and tasks | CODE-01, CONTEXT-01, TASK-01, CACHE-01 | Active-file admission, build/config files, diversified results, safe reopen reads, context/snapshot cache dependencies, task/turn distinction | AST/LSP, incremental repo map, evidence selection trace, acceptance graph and independently validated task success |
| 6. Memory validity | MEMORY-01, MEMORY-02 | Confidence basis (unknown/heuristic/measured), exact-event cursor search | Persistent index, granular support/contradiction/revocation, dependency validity, retention/export/delete, owner authorization |
| 7. Service and enforcement | SERVER-01, SECURITY-01, discovery part of TOOLS-01 | MCP submit/poll/cancel/release, cursor gaps, bounded event retention, edit conflict results | Durable resume, approvals, persistent processes, policy-consistent discovery, platform enforcement matrix, fuzzing/TSan |
| 8. Measured policies | EVAL-02, ROUTING-01, EXPERIENCE-01, SEARCH-01, OPTIMIZE-01 | Repeats, isolated engine state, protected checks, Wilson interval, p50/p95, sampled process-tree RSS, no implicit calibration promotion | Real-model/hardware baseline and holdouts; measured routing, reusable procedures and candidate-search experiments |
| 9. Adoption | INTEROP-01, PRODUCT-01, ADOPTION-01 | Read-only `--doctor`, accurate build/accounting documentation | ACP, SDKs, MCP client, signed packages, editor flows and external user trials |

No experimental routing or procedure promotion is enabled on the strength of
fake-model tests. Paid APIs, signing credentials, hardware measurements and user
trials require actual resources. No real-model result has been produced here.

## Implemented contracts

- A read cannot certify a patch. Only closed project verification workflows can
  produce an accepted receipt. The engine parses typed fields, records the action
  ID and workspace fingerprint, and checks freshness again before answering and
  committing. A mutation invalidates prior proof even if the tool later fails.
- CMake/pytest receipts use JUnit collection counts. Empty/all-skipped suites,
  missing reports and truncated logs do not pass. npm no longer uses
  `--if-present`. Cargo/npm tests remain inconclusive without collection evidence.
  The result retains actual argv and stdout/stderr, including truncation flags.
- Oracle fixtures are rebuilt independently. Original acceptance files and build
  inputs cannot be patched. Added tests are audited and retained in artifacts,
  but cannot alter the protected oracle. Hidden checks are materialized only in
  the verifier workspace. Production evaluation requires Linux bubblewrap with
  read-only fixture/toolchains and isolated networking; deterministic unit tests
  explicitly bypass isolation only for evaluator-authored fixtures.
- asmodel owns backend residency and locks. Session lanes borrow the same manager;
  the current embedded wrapper serializes requests to one backend instance.
  This is shared residency, not native multi-sequence decoding. The former
  duplicate engine LRU/load machinery was removed.
- Remote token counting is explicitly estimated. Exact status requires a
  successful template-aware count and tokenizer/template identities. The current
  fallback admission margin is conservative but uncalibrated. Per-request
  generation info carries usage-known, partial output and diagnostics.
- Operation reservations are durable before inference, including shared Asper
  generation and embeddings. Unknown usage retains its reservation. Conversation
  rollback cannot refund consumption. Replay validates reserve/settle identities
  and rejects double settlement. Session-lifetime cost and monetary reconciliation
  still need a dedicated operation projection.
- The owner store has a single-writer lock. Action/consumption WALs have versioned
  length/SHA-256 framing, per-frame and total-log quotas. Complete corrupt frames
  fail closed; only incomplete final frames are repaired. Short writes and flush
  errors roll back; uncertain fsync closes the stream. POSIX replacement syncs the
  parent directory. This is not a demonstrated power-loss guarantee on every OS.
- `code.read-range` returns SHA-256 of the whole original file. Edits can require
  that version; patches can require versions for every target. Results identify
  changed files and before/after versions. Rechecks precede replacement; rollback
  preserves intervening external edits. This is optimistic conflict detection,
  not atomic compare-and-swap against arbitrary editors.
- Response cache keys include conversation, objective, active file, prompt,
  security profile and current workspace. Response reuse is disabled while Asper
  is active because a verifiable memory revision is not yet available.
- MCP jobs retain at most 256 events and 32 handles; poll reports cursor gaps.
  The service returns `task_state: unconfirmed`, including after a successful turn.
  Process restart does not preserve these event rings.
- `--doctor` reads inputs only. Remote connectivity and model loading are explicitly
  unprobed; missing weights/configuration/credentials and ABI mismatches are visible.

## Validation at this checkpoint

- Integrated no-llama suite: 30/30 CTest executables passed.
- Integrated TUI/MCP build with ASan/UBSan/LeakSanitizer: 30/30 passed.
- Standalone Asper: 22/22; astools: 24/24; asmodel: 2/2.
- Five oracle regressions passed: changed original tests rejected, cosmetic patch
  still fails, real recursive repair passes, new tests cannot disable originals,
  arbitrary startup code additions rejected.
- Both repaired fixtures passed the actual bubblewrap verifier, including hidden
  checks. A verifier-side attempt to write the protected workspace was refused.
- Actual `project.test` integration exercised a CTest suite that ran, a disabled
  suite and an empty suite. This exposed and fixed multiline JUnit parsing that
  the initial synthetic test did not cover.
- Fault cases cover stale snapshots, external symlinks, stale edit versions,
  interrupted turns, incomplete WAL tails, valid-text checksum corruption,
  short write, flush/fsync failure, unknown usage and duplicate settlement.

The asmodel mock needs a loopback socket and LeakSanitizer needs process
inspection, so these checks ran outside the tool sandbox. The earlier parallel
astools process-headroom sampling race is not an enforcement guarantee; the
complete sequential suite passes. Windows/macOS, embedded llama and real-model
behavior have not been validated by these Linux no-llama runs.

## Next implementation order

1. Reconstruct/build the exact local release, then finish interruptible model
   requests and the typed protocol before extending adapters.
2. Add persistent task acceptance state and evidence dependencies; then memory
   validity/revocation and indexed history.
3. Add discovery, resumable approvals, controlled processes and editor protocols.
4. Run the real-model matrix with supplied configuration before admitting learned
   routing, reusable procedures, alternative patches or offline policy promotion.
