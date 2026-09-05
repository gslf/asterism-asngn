# Quality execution plan

This tracker distinguishes implementation, contract tests and model evaluation.
It does not claim that the full research/product program is complete.
Baseline on 2026-09-05: asngn 8869910, asper 5295ced, asmodel 5ccdc3b,
astools efa6d22. Local source changes are included in the tested builds.

## Milestones

| Milestone | Work items | Implemented and tested | Remaining gate |
|---|---|---|---|
| 1. Trustworthy outcomes | VERIFY-01, EVAL-01, verifier part of TOOLS-01 | Typed receipts, action/snapshot binding, stale-proof rejection, test collection, independent protected oracle | Expand adapters, toolchain identity and protected repository task suite |
| 2. Reproducible foundation | RELEASE-01, TOKENS-01, USAGE-01 | Release manifest, ABI/header checks, standalone and reconstructed clean builds, explicit token uncertainty, durable operation reservations | Published pins, calibrated remote tokenizer margins |
| 3. Safe state | WORKSPACE-01, STORAGE-01, ACTIONS-01, CONCURRENCY-01 | Descriptor-relative reads, bounded snapshots, expected edit hashes, writer lock, framed WAL/checksums, I/O fault tests | Incremental snapshots, durable approval recovery, compaction/migration, cross-process workspace coordination |
| 4. Runtime contract | RUNTIME-01, PROTOCOL-01, PROVIDERS-01, EMBED-01 | One asmodel residency owner across lanes, intact cancellation/errors/partial output, per-request usage, cancellable generation queues, explicit output schemas, embedding batches/receipts, shared versioned preprocessing and remaining deadlines | Role/block message IR, native sequence batching, real provider conformance and turn-wide memory cancellation |
| 5. Evidence and tasks | CODE-01, CONTEXT-01, TASK-01, CACHE-01 | Active-file admission, build/config files, diversified results, safe reopen reads, context/snapshot cache dependencies, persistent host acceptance graph, task/turn distinction | AST/LSP, incremental repo map, evidence selection trace, fine-grained dependencies and task hypotheses |
| 6. Memory validity | MEMORY-01, MEMORY-02 | Confidence basis (unknown/heuristic/measured), indexed cursor search, checked event frames, single-writer store | Inverted text index, granular support/contradiction/revocation, dependency validity, retention/export/delete, owner authorization |
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
- Output contracts now travel explicitly through asmodel ABI 5. The remote
  provider no longer identifies or rewrites engine/memory protocols by inspecting
  GBNF. Asngn owns action/classification/judge schemas and validation; Asper owns
  its curation/review/recall schema and exact output wrapper. Astools exports typed
  command argument schemas through the same registry selector as GBNF/catalog.
  JSON output metadata survives both shared-runtime adapters. Metadata strings
  preserve quotes/Unicode and reject oversized values instead of truncating intent.
  This completes the output-contract slice, not the multimodal/message IR.
- Embedding batches retain valid leading rows, explicit consumption and cancellation/
  timeout errors. The shared manager owns query/document prefixes for both hosts.
  Canonical cache keys include revision, tokenizer, pooling, dimension, context
  and preprocessing; unknown remote revisions invalidate persisted vectors on
  restart. Native inputs are rejected when too long, never silently truncated.
  Retrieval embeds up to 32 active/lexically relevant candidates in one batch.
  This selection remains an uncalibrated policy, with lexical fallback on failure.
- Remote token counting is explicitly estimated. Exact status requires a
  successful template-aware count and tokenizer/template identities. The current
  fallback admission margin is conservative but uncalibrated. Per-request
  generation info carries usage-known, partial output and diagnostics.
- Operation reservations are durable before inference, including shared Asper
  generation and embeddings. Unknown usage retains its reservation. Conversation
  rollback cannot refund consumption. Replay validates reserve/settle identities
  and rejects double settlement. Session-lifetime cost and monetary reconciliation
  still need a dedicated operation projection.
- The owner store has a single-writer lock. Action/consumption WALs have
  version-2 length/header/payload SHA-256 framing, per-frame and total-log quotas. Complete corrupt frames
  fail closed; only incomplete final frames are repaired. Short writes and flush
  errors roll back; uncertain fsync closes the stream. POSIX replacement syncs the
  parent directory. This is not a demonstrated power-loss guarantee on every OS.
- `code.read-range` returns SHA-256 of the whole original file. Edits can require
  that version; patches can require versions for every target. Results identify
  changed files and before/after versions. Rechecks precede replacement; rollback
  preserves intervening external edits. This is optimistic conflict detection,
  not atomic compare-and-swap against arbitrary editors.
- Asper stores AEV2 frames with separate metadata/payload checksums and bounded
  event/log sizes. Cursor pages use a rebuildable offset index and one temporary
  frame; literal search can still scan the remaining tail. Full-list callers
  allocate the full result. A page validates returned events, not unrelated
  earlier frames. Existing AEV1 logs are not silently migrated.
- Response cache keys include conversation, objective, active file, prompt,
  security profile and current workspace. Response reuse is disabled while Asper
  is active because a verifiable memory revision is not yet available.
- Host acceptance contracts persist independently of turns, with revision checks,
  bounded prerequisite graphs, snapshot-bound runtime proofs and conservative
  invalidation. Mandatory criteria survive context trimming. MCP exposes contract
  definition/read/invalidation and per-criterion state; old jobs report superseded
  after definition changes. See [acceptance contracts](acceptance.md) for coverage
  limits and the required host invalidation after external toolchain changes.
- MCP jobs retain at most 256 events and 32 handles; poll reports cursor gaps.
  Without a host acceptance contract, a committed turn remains `unconfirmed`.
  Process restart does not preserve these event rings.
- `--doctor` reads inputs only. Remote connectivity and model loading are explicitly
  unprobed; missing weights/configuration/credentials and ABI mismatches are visible.

## Validation at this checkpoint

- Native CPU build against the pinned llama.cpp submodule compiles the actual
  adapters and passes 32/32 fake-based tests; no weights were loaded.
- Integrated no-llama suite: 32/32 CTest executables passed.
- Integrated TUI/MCP build with ASan/UBSan/LeakSanitizer: 32/32 passed.
- Standalone Asper: 22/22; astools: 24/24; asmodel: 5/5.
- The shared strict JSON codec replaces protocol substring parsing. Provider
  tests reject misplaced usage counters, duplicate keys, invalid vector indices,
  non-finite/wrong-size vectors and incomplete SSE. Standalone asmodel also passes
  all five executables with ASan/UBSan/LeakSanitizer.
- Embedding regressions cover reordered remote batches, queue expiry, cancellation,
  partial results, invalid vectors, immutable pipeline fields and identity changes.
  A shared-host test proves that Asper and Asngn use the host prefixes exactly once;
  a 48-file retrieval fixture admits an active file and a late lexical candidate.
- Explicit-schema mock tests pass without recognized grammar text; unsupported
  constraints and requests exceeding schema-inclusive admission fail before HTTP.
- An integrated JSON-provider fixture crosses the shared manager, decodes a real
  tool decision and invokes the tool process. Scalar/object corruption cases and
  disabled-tool schema exports are covered.
- Five oracle regressions passed: changed original tests rejected, cosmetic patch
  still fails, real recursive repair passes, new tests cannot disable originals,
  arbitrary startup code additions rejected.
- Both repaired fixtures passed the actual bubblewrap verifier, including hidden
  checks. A verifier-side attempt to write the protected workspace was refused.
- Actual `project.test` integration exercised a CTest suite that ran, a disabled
  suite and an empty suite. This exposed and fixed multiline JUnit parsing that
  the initial synthetic test did not cover.
- Clean local clones of all four repositories built and passed all four suites,
  including the integrated TUI. Pins are local commits; publication is not claimed.
- Exact memory tests cover deleted/corrupt offset indices, late cursor pages,
  payload corruption and altered lengths that must not trigger truncation,
  in both memory event frames and the action/consumption WAL.
- Acceptance tests cover prerequisite order, failed prerequisites, empty/malformed
  receipts, selective definition changes, persisted revocation, external edits,
  session reopen, I/O failure and MCP revision/status transport. An integrated
  async turn includes mandatory criteria and remains incomplete after committing.
  A real stdio MCP process defines, reads and invalidates contracts across restart.
- Fault cases cover stale snapshots, external symlinks, stale edit versions,
  interrupted turns, incomplete WAL tails, valid-text checksum corruption,
  short write, flush/fsync failure, unknown usage and duplicate settlement.

The asmodel mock needs a loopback socket and LeakSanitizer needs process
inspection, so these checks ran outside the tool sandbox. The earlier parallel
astools process-headroom sampling race is not an enforcement guarantee; the
complete sequential suite passes. Windows/macOS, embedded llama and real-model
behavior have not been validated by these Linux no-llama runs.

## Next implementation order

1. Extend output contracts to role/block messages and finish embedding requests before extending
   provider adapters. Native loader interruption remains backend-dependent.
2. Extend acceptance state with file/toolchain dependencies and task hypotheses;
   then add memory validity/revocation and indexed history.
3. Add discovery, resumable approvals, controlled processes and editor protocols.
4. Run the real-model matrix with supplied configuration before admitting learned
   routing, reusable procedures, alternative patches or offline policy promotion.
