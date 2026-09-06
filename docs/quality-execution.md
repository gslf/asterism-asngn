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
| 3. Safe state | WORKSPACE-01, STORAGE-01, ACTIONS-01, CONCURRENCY-01 | Shared authorized enumeration, global snapshot quotas, streaming file hashes, observed scan-conflict detection, expected edit hashes, writer lock, framed WAL/checksums, checked memory snapshots, validated compaction backups, I/O and compaction crash tests, durable bound approvals | Incremental snapshots/ignore syntax, durable task recovery, explicit data conversion, cross-process workspace coordination |
| 4. Runtime contract | RUNTIME-01, PROTOCOL-01, PROVIDERS-01, EMBED-01 | One asmodel residency owner across lanes, intact cancellation/errors/partial output, per-request usage, cancellable generation queues, explicit output schemas, role/block input, remote native tool proposals, policy-bound native action loop and validated final-response reuse, embedding batches/receipts, shared versioned preprocessing and remaining deadlines | Attachments, native sequence batching, real provider conformance and turn-wide memory cancellation |
| 5. Evidence and tasks | CODE-01, CONTEXT-01, TASK-01, CACHE-01 | Active-file admission, build/config files, diversified results, optional managed clangd navigation, direct UTF-8 blob ranges, late diagnostic excerpts, bounded context/evidence selection traces, context/snapshot cache dependencies, persistent host acceptance graph, task/turn distinction | AST/incremental repo map, dependency-fresh LSP coverage, ranked role coverage, granular Asper/native-request traces, fine-grained dependencies and task hypotheses |
| 6. Memory validity | MEMORY-01, MEMORY-02 | Confidence basis (unknown/heuristic/measured), indexed cursor search, checked event frames, bounded hash-verified object slices, progressive bounded source context, single-writer store, granular source ranges, dependency validity, support/conflict/correction links, retained revision history, checked offline whole-store export and resumable erasure, durable source-curation receipts and explicit partial-outcome reconciliation | Inverted text index, curator-proposed spans, selective retention/erasure, cleanup outside the store, authenticated owner APIs |
| 7. Service and enforcement | SERVER-01, SECURITY-01, discovery part of TOOLS-01 | MCP submit/poll/cancel/release, cursor gaps, bounded event retention, edit conflict results, policy-filtered command snapshots, model-facing discovery, checked cancellable tool queues, durable approval inspection, package-bound persistent runtime | Durable resume, interactive process control, discovery quality measurements, platform enforcement matrix, fuzzing/TSan |
| 8. Measured policies | EVAL-02, ROUTING-01, EXPERIENCE-01, SEARCH-01, OPTIMIZE-01 | Repeats, isolated engine state, protected checks, Wilson interval, p50/p95, sampled process-tree RSS, no implicit calibration promotion | Real-model/hardware baseline and holdouts; measured routing, reusable procedures and candidate-search experiments |
| 9. Adoption | INTEROP-01, PRODUCT-01, ADOPTION-01 | Read-only `--doctor`, Python and JavaScript/TypeScript host SDKs, tested local packages, accurate build/accounting documentation | ACP, general MCP client, signed packages, editor flows and external user trials |

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
  explicitly bypass isolation only for evaluator-authored fixtures. Schema 3 also
  requires invocation-bound positive completion with expected test counts: an
  early zero exit, empty collection or skipped suite cannot certify a repair.
  Commands/output/patch capture are bounded; dependent checks stop on failure.
  Public smoke tasks are always labelled `dev`, never `holdout`. These receipts
  do not make arbitrary in-process test code unable to forge its own output.
- asmodel owns backend residency and locks. Session lanes borrow the same manager;
  the current embedded wrapper serializes requests to one backend instance.
  This is shared residency, not native multi-sequence decoding. The former
  duplicate engine LRU/load machinery was removed.
- Execution responsibilities are separate modules: the turn loop coordinates
  ingestion/commit, while action application, tools, confirmations, drafts and
  responses share explicit private boundaries. Structured generation preserves
  roles/tool-result blocks through the watchdog and reserves schema costs before
  dispatch. An explicit native action policy uses the generator and the same
  execution gates; correlated current-turn tool results retain native roles.
  Whole-batch preflight rejects invalid arguments, mixed mutations and budgets
  before the first effect. Native inline payloads avoid the draft pass. A native
  text proposal can now reuse the response validation path without a separate
  generation, retaining configured classification, output gates and review. See
  [native actions](native-actions.md) for bounds and unmeasured costs.
- Output contracts now travel explicitly through asmodel ABI 7. The remote
  provider no longer identifies or rewrites engine/memory protocols by inspecting
  GBNF. Asngn owns action/classification/judge schemas and validation; Asper owns
  its curation/review/recall schema and exact output wrapper. Astools exports typed
  command argument schemas through the same registry selector as GBNF/catalog.
  JSON output metadata survives both shared-runtime adapters. Metadata strings
  preserve quotes/Unicode and reject oversized values instead of truncating intent.
  Message and native tool contracts now preserve roles and correlation IDs;
  attachments remain. Embedded text templates
  reject unsupported blocks instead of silently flattening or switching templates.
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
- Workspace fingerprinting and retrieval share an authorized tree walk and ignore
  policy. Global quotas apply before file callbacks, even within flat directories.
  Hashing streams through 8 KiB; failed or incomplete snapshots expose no fingerprint.
  Retrieval attaches full-file/chunk hashes and exact ranges and reports partial
  scans. See [workspace boundaries](workspace-tree.md) for the remaining incremental,
  ignore-pattern, hard-link and platform limits.
- Asper record store format 2 rejects legacy/unframed data, complete corruption and
  impossible replay transitions. Checked snapshots and compaction markers bind
  backups by hash. Recovery validates all backups before restoring any target;
  uncertain sync blocks retrieval, mutations and compaction until reopen.
- Asper stores AEV2 frames with separate metadata/payload checksums and bounded
  event/log sizes. Cursor pages use a rebuildable offset index and one temporary
  frame; literal search can still scan the remaining tail. Full-list callers
  allocate the full result. A page validates returned events, not unrelated
  earlier frames. Existing AEV1 logs are not silently migrated.
- Grounding binds claim hashes to exact event intervals and versioned dependencies.
  Support validity propagates to derivatives; conflicts, revocations and corrections
  remove affected memories from retrieval. Runtime observations are renewed after
  restart. Checked history retains earlier claim text across record compaction.
  The engine observes the session workspace before memory materialization. See
  [Asper grounding](../../asterism-asper/docs/knowledge.md) for semantics and limits.
- Optional clangd navigation uses a closed LSP contract on the existing process
  runtime. Source SHA-256 preconditions, UTF-8 position negotiation, exact-version
  diagnostics and final source rechecks prevent stale source observations from
  becoming successful replies. Host reads reject symlink ancestors and out-of-scope
  locations. Server requests cannot edit or execute host actions. Errors remain
  attached to the request through MCP. Target hashes identify current host reads;
  headers, server indexes and toolchain freshness are not certified. These results
  never count as verification receipts. See [LSP limits and setup](../../asterism-astools/docs/lsp.md).
- Response cache keys include conversation, objective, active file, prompt,
  security profile and current workspace. Response reuse is disabled while Asper
  is active because a verifiable memory revision is not yet available.
- Host acceptance contracts persist independently of turns, with revision checks,
  bounded prerequisite graphs, snapshot-bound runtime proofs and conservative
  invalidation. Mandatory criteria survive context trimming. MCP exposes contract
  definition/read/invalidation and per-criterion state; old jobs report superseded
  after definition changes. See [acceptance contracts](acceptance.md) for coverage
  limits and the required host invalidation after external toolchain changes.
- Evidence views scan the entire redacted result for first and recent diagnostics,
  retaining exact UTF-8 byte ranges. Project/process logs need no compressor;
  other digests cannot drop recognized diagnostic excerpts. Explicit range objects
  replace implicit blob cursors across GBNF, JSON Schema and native controls.
  Selection traces identify versions, costs, omissions and reasons without copying
  source text; delegated Asper materialization is not mislabeled as a budget trim.
  Ring/batch byte quotas bound telemetry growth. See [evidence and context](evidence-context.md)
  for diagnostic coverage, attribution uncertainty and trace scope limitations.
- A turn owns one bounded command snapshot for its prompt, GBNF, JSON Schema and
  actual invocation. `discover` replaces that snapshot by searching all statically
  permitted candidates; unavailable commands cannot bypass selection. Read-only
  profiles omit mutating commands. Runtime preflight precedes cached-result reuse
  and confirmation; package hashes enter cache keys. Post-queue checks reject
  revoked or changed packages, and tool waits now receive turn cancellation.
  Selection quality is lexical and uncalibrated; file-check/spawn is not atomic.
  Obsolete catalog/grammar/annotation caches and catalog-level configuration were
  removed. See [tool selection](tool-selection.md).
- Interactive approvals bind effective arguments, package, workspace and profile.
  Decisions are durable before execution; changed snapshots invalidate review.
  The TUI scrolls full redacted arguments, and MCP exposes read-only inspection.
  Reopening interrupted approvals never replays actions; durable task resumption
  remains separate work. See [approval contracts](approvals.md).
- Persistent tool instances retain their own sandbox and scratch directory, use
  immutable package identity, and recheck queued work before input reaches the
  process. Instance queues are cancellable and observable; handshake deadlines
  include request admission. Native libraries reject changed loaded packages and
  validate response IDs. See [persistent runtime](../../asterism-astools/docs/persistent-runtime.md).
  This does not yet expose raw shell/REPL sessions or durable process resumption.
- MCP jobs retain at most 256 events and 32 handles; poll reports cursor gaps.
  Without a host acceptance contract, a committed turn remains `unconfirmed`.
  Process restart does not preserve these event rings.
- Python and JavaScript/TypeScript SDKs negotiate Asterism wire contract 1 and
  expose live tasks, cursor pages, cancellation, acceptance revisions and read-only
  approvals. They preserve terminal failure/incomplete states and late-reply IDs;
  message, pending-request and stderr quotas bound payload retention. They own
  and close the local server process. No automatic effect retry or durable task
  resume is implied. See [SDK contracts](../sdk/README.md) for platform and lifecycle
  limits. Both archives install and work outside the checkout; no runtime package
  dependencies or registry publication are implied.
- `--doctor` reads inputs only. Remote connectivity and model loading are explicitly
  unprobed; missing weights/configuration/credentials and ABI mismatches are visible.

## Validation at this checkpoint

- Native CPU build against the pinned llama.cpp submodule compiles the actual
  adapters and passes 46/46 fake-based tests; no weights were loaded.
- Integrated no-llama suite: 46/46 CTest executables passed.
- Integrated TUI/MCP build with ASan/UBSan/LeakSanitizer: 46/46 passed.
- Standalone Asper: 30/30; astools: 32/32; asmodel: 7/7.
- The shared strict JSON codec replaces protocol substring parsing. Provider
  tests reject misplaced usage counters, duplicate keys, invalid vector indices,
  non-finite/wrong-size vectors and incomplete SSE. Standalone asmodel also passes
  all seven executables with ASan/UBSan/LeakSanitizer.
- The native controller also passes a full MCP/HTTP/tool-process integration for
  Chat Completions and Responses with scripted peers. Provider cancellation remains
  cancellation unless the watchdog actually observed a stall. Five additional
  native-response cases cover Unicode, output withheld before validation, protocol
  rejection, response budgets, review rejection, current/stale receipts, artifact
  and empty-output gates, and reuse without another token charge. Both HTTP shapes
  produce the same scripted result in three requests with native final text versus
  four with explicit finish and a separate response. This proves one omitted
  request in the fixture; real-model cost, latency and quality remain unmeasured.
- Message/tool tests exercise full HTTP encodings, streamed arguments, unknown/
  duplicate/reused IDs, absent required tools and malformed JSON. Failed or
  incomplete generations cannot expose tool proposals for execution.
- Generation mock tests retain decoded prefixes after timeout/cancellation,
  distinguish unknown usage, and prove that a later call cannot overwrite an
  earlier receipt. Native adapters compile with the same per-request contract;
  real-weight behavior remains unmeasured. Recall tests check remaining durations.
- Embedding regressions cover reordered remote batches, queue expiry, cancellation,
  partial results, invalid vectors, immutable pipeline fields and identity changes.
  A shared-host test proves that Asper and Asngn use the host prefixes exactly once;
  a 48-file retrieval fixture admits an active file and a late lexical candidate.
- Explicit-schema mock tests pass without recognized grammar text; unsupported
  constraints and requests exceeding schema-inclusive admission fail before HTTP.
- An integrated JSON-provider fixture crosses the shared manager, decodes a real
  tool decision and invokes the tool process. Scalar/object corruption cases and
  disabled-tool schema exports are covered.
- Twenty oracle/process regressions pass, including original-test/header tampering,
  unchanged bugs, real C/Python repairs, early zero exits, disabled/skipped tests,
  mismatched or duplicate completion records, binary/oversized patches, special
  input files, output floods, deadlines and orphaned output pipes. Both repaired
  fixtures and Python early-exit rejection also run inside actual bubblewrap.
- Both repaired fixtures passed the actual bubblewrap verifier, including hidden
  checks. A verifier-side attempt to write the protected workspace was refused.
- Actual `project.test` integration exercised a CTest suite that ran, a disabled
  suite and an empty suite. This exposed and fixed multiline JUnit parsing that
  the initial synthetic test did not cover.
- Clean local clones of all four repositories built and passed all four suites,
  including the integrated TUI. Pins are local commits; publication is not claimed.
- Record-storage tests interrupt a real process at five compaction boundaries,
  verify non-duplicated replay, and ensure a damaged later backup leaves earlier
  targets untouched. Snapshot truncation, legacy versions and uncertain sync fail
  closed. POSIX parent directories are synced; this is not a power-loss certification.
- Exact memory tests cover deleted/corrupt offset indices, late cursor pages,
  payload corruption and altered lengths that must not trigger truncation,
  in both memory event frames and the action/consumption WAL.
- Source objects now hash the complete content while allocating only the requested
  range. Corruption outside that range, oversized/empty objects, aliased buffers,
  symlinked blobs and FIFOs are covered. This reduces slice allocation but adds
  complete hashing per read; no end-to-end speedup is claimed.
- Linux offline store maintenance adds 15 cases for physical export/verification,
  real MCP lock contention in both directions, restore of records/source events,
  stale snapshot rejection, malformed metadata, copy corruption, external changes,
  quotas, aliases, mount-identity checks, interrupted erasure and uncertain sync.
  The runtime checks a persistent erasure guard before recovery or file logging.
  Erasure includes every derivative and backup inside the root; selective retention,
  stores/logs outside it and authenticated multi-user APIs remain separate. See
  [data governance](../../asterism-asper/docs/data-governance.md).
- Progressive history tests cover old pins, recent tails, non-additive token counts,
  zero-count callbacks, byte/event caps, corrupt checkpoints, callback reentry,
  captured prefixes/pins, external truncation/rewrites/unlinking, payload corruption
  and curation acknowledgement capacity before inference. A five-repeat component
  probe returns identical context with median process RSS 137,836 to 24,784 KiB
  and wall time 1.452 to 0.712 seconds on a warm 128 MiB Linux fixture. This is
  not model evaluation; skipped metadata is not verified payload evidence. Pending
  source admission is now bounded; source-driven mutation batches use durable
  receipts and explicit reconciliation of interrupted partial outcomes.
  See [source context and raw measurements](../../asterism-asper/docs/source-context.md).
- Source-curation receipts pass seven process-crash boundaries, seven live I/O
  boundaries, uncertain journal sync under every policy, bounded full flush and
  quota checks. Six offline integration cases cross a real interrupted C batch,
  operator snapshot review, Unicode notes and MCP restart. A shared probe against
  the previous library reproduces re-proposal: two source events and one new model
  call after a partial insertion; the receipt runtime retains that insertion,
  suspends the batch and makes no further call. This is conservative reconciliation,
  not atomic batch rollback or a task-success claim. Standalone no-thread Asper
  also passes 30/30. See [curation recovery](../../asterism-asper/docs/curation-recovery.md).
- Curation now selects complete inputs before retrieval or generation, retaining
  the omitted tail and checking the joined transcript against token and byte
  limits. Four cases cover exact receipt membership, failure with concurrent
  append, oversized events and non-additive/zero/missing counters. The same first
  case against the previous library reproduces seven silently omitted inputs out
  of twenty; the revised runtime sends all twenty once across three bounded calls.
  Oversized head events stop with `LIMIT` and remain pending; segmentation remains
  open. This is a contract regression, not model quality.
- Asper ABI 7 bounds admitted and in-flight source text to 32 MiB and the configured
  event limit, retaining excess inputs on disk behind per-scope cursors. Retry
  restores reserved slots without allocating another array. Full flush captures
  endpoints, so concurrent appends cannot extend that drain indefinitely. Eleven
  cases cover restart, exact eventual coverage, four producers, the live worker,
  byte pressure, corrupt sources, project changes and invalid/missing origin
  objects. Default origins capture project identity before deferral; replay never
  guesses the host's later active project. C/shared-library/MCP checks expose queue,
  in-flight, byte, backlog and pending-receipt observations. This is not a whole
  process memory quota; source tables and temporary reads retain separate bounds.
  See [curation admission](../../asterism-asper/docs/curation-queue.md).
- Release admission now tests actual temporary Git checkouts: stale standalone
  dependencies, dirty or replaced submodules, missing required checkouts and
  header changes cannot pass via the engine's development exception. All four
  standalone Asper jobs and the real-model smoke read declared pins. Standalone
  asmodel GCC and Clang/sanitizer recipes each pass 7/7 with mandatory HTTP coverage.
  YAML was parsed locally; remote CI and the real-model smoke have not run here.
- Grounding tests cover bad UTF-8 ranges, stale hashes/revisions, support cycles,
  partial coverage, changed dependencies, missing source events, contradictory
  claims, revocations, history cursors, uncertain writes and complete corruption.
  An engine fixture uses a separate checkout and confirms that a workspace edit
  removes the stale memory from the next prompt.
- Acceptance tests cover prerequisite order, failed prerequisites, empty/malformed
  receipts, selective definition changes, persisted revocation, external edits,
  session reopen, I/O failure and MCP revision/status transport. An integrated
  async turn includes mandatory criteria and remains incomplete after committing.
  A real stdio MCP process defines, reads and invalidates contracts across restart.
- Approval tests cover expanded draft payloads, changed packages/snapshots,
  immutable records, rejected decision writes, reopen without replay, read-only
  MCP inspection and scrolling through long Unicode review text.
- Ten persistent-runtime cases and two real-library cases pass. Seven lifecycle
  regressions also fail against the previous runtime: stale executable reuse,
  deleted scratch, delayed deadlines/cancellation and surviving descendants. The
  no-thread Astools build passes 28/28 (persistent execution is unavailable there).
- Ten evidence cases cover late and dense diagnostics, compressor omissions,
  exact redacted offsets, invalid UTF-8, strict native/JSON range contracts and
  reads that cannot certify success. The native build also checks actual pinned
  llama.cpp grammar acceptance. Context traces are deterministic, omit source text
  and report capped detail; a pressure test retains whole file events while
  bounding ring/batch bytes.
- Eleven workspace cases cover traversal and byte quotas, descriptor containment,
  changed files/directories, aliases/FIFO, stable content versions and the corpus
  cap. A separate probe against the previous committed build reproduces the flat
  directory bug: 264 MiB was accepted despite the 256 MiB limit; the new snapshot
  fails with no fingerprint. The Windows handle walker remains unvalidated here.
- SDK checks include nine Python and eight JavaScript transport/lifecycle cases,
  TypeScript compilation, and actual MCP/HTTP integration in each language. They
  cover Unicode, event truncation, local timeout versus engine cancellation,
  pending approvals over 4 KiB with exact hashes, denied self-approval, persisted
  interruption, stale work revisions and process-local task handles. The server
  rejects oversized requests; malformed peers, late replies and remaining process
  group members cannot silently corrupt client state. Python wheel and npm archive
  checks use isolated installs, live MCP calls and installed TypeScript exports.
  Local interpreters were Python 3.14.7/3.12 and Node 26.7.0; the compiler was 7.0.2.
  CI requests Node 22 and locks its test tooling; other platform runs are not claimed.
- LSP checks include real clangd 22.1.8 overloads, Unicode byte columns, references,
  diagnostics and Linux strict queries. Hostile peers exercise duplicate keys,
  wrong IDs, stale diagnostics, in-query source changes, external locations,
  server requests, deadlines, cancellation, clean restart and bounded Unicode
  errors. The optional package test checks the complete generated manifest,
  disabled repository config, MCP discovery and output-schema validation.
- Fault cases cover stale snapshots, external symlinks, stale edit versions,
  interrupted turns, incomplete WAL tails, valid-text checksum corruption,
  short write, flush/fsync failure, unknown usage and duplicate settlement.

The asmodel mock needs a loopback socket and LeakSanitizer needs process
inspection, so these checks ran outside the tool sandbox. Astools' process-limit selector now exposes the same observation used to derive
its headroom, removing the test's race between separate desktop task samples.
The observation still is not a per-tree process quota guarantee. Windows/macOS, embedded llama and real-model
behavior have not been validated by these Linux no-llama runs.

## Next implementation order

1. Extend the native contract with attachments only with honest modality admission;
   measure validated native-response reuse with real models. Native loader
   interruption remains backend-dependent.
2. Extend acceptance state with file/toolchain dependencies and task hypotheses;
   add retention/export/delete and explicit owner authorization to memory.
3. Add durable task resumption, controlled processes and editor protocols.
4. Run the real-model matrix with supplied configuration before admitting learned
   routing, reusable procedures, alternative patches or offline policy promotion.
