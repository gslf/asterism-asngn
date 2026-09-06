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
| 3. Safe state | WORKSPACE-01, STORAGE-01, ACTIONS-01, CONCURRENCY-01 | Shared authorized enumeration, global snapshot quotas, bounded Git identity and registered worktree resolution, streaming file hashes, observed scan-conflict detection, expected edit hashes, writer lock, framed WAL/checksums, checked memory snapshots, validated compaction backups, I/O and compaction crash tests, durable bound approvals, streamed recovery and durable terminal task observations | Incremental snapshots/ignore syntax, other Git metadata layouts, effect reconciliation and resume, explicit data conversion, cross-process workspace coordination |
| 4. Runtime contract | RUNTIME-01, PROTOCOL-01, PROVIDERS-01, EMBED-01 | One asmodel residency owner across lanes, intact cancellation/errors/partial output, per-request usage, cancellable generation queues, explicit output schemas, role/block input, remote native tool proposals, policy-bound native action loop and validated final-response reuse, embedding batches/receipts, shared versioned preprocessing and remaining deadlines | Attachments, native sequence batching, real provider conformance and turn-wide memory cancellation |
| 5. Evidence and tasks | CODE-01, CONTEXT-01, TASK-01, CACHE-01 | Active-file admission, query-ranked bounded corpus across continued scans, build/config files, diversified results, optional managed clangd navigation, direct UTF-8 blob ranges, late diagnostic excerpts, bounded context/evidence selection and native-request traces, generation trace/consumption correlation, context/snapshot cache dependencies, persistent host acceptance graph, task/turn distinction | AST/incremental repo map, dependency-fresh LSP coverage, ranked role coverage, granular Asper and embedding traces, fine-grained dependencies and task hypotheses |
| 6. Memory validity | MEMORY-01, MEMORY-02 | Confidence basis (unknown/heuristic/measured), indexed cursor search, checked event frames, bounded hash-verified object slices, progressive bounded source context, single-writer store, granular source ranges, dependency validity, support/conflict/correction links, retained revision history, checked offline whole-store export and resumable erasure, durable source-curation receipts, explicit partial-outcome reconciliation and reversible source deferral | Inverted text index, curator-proposed spans, selective retention/erasure, cleanup outside the store, authenticated owner APIs |
| 7. Service and enforcement | SERVER-01, SECURITY-01, discovery part of TOOLS-01 | MCP submit/poll/cancel/release, cursor gaps, bounded event retention, edit conflict results, policy-filtered command snapshots, model-facing discovery, checked cancellable tool queues, durable approval inspection, package-bound persistent runtime, archived task retrieval through MCP/SDKs, instrumented shared JSON/provider and tool JSON/manifest/schema fuzz targets | Durable resume, interactive process control, discovery quality measurements, platform enforcement matrix, storage/process fuzzing and TSan |
| 8. Measured policies | EVAL-02, ROUTING-01, EXPERIENCE-01, SEARCH-01, OPTIMIZE-01 | Repeats, isolated engine state, protected checks, Wilson interval, p50/p95, sampled process-tree RSS, no implicit calibration promotion | Real-model/hardware baseline and holdouts; measured routing, reusable procedures and candidate-search experiments |
| 9. Adoption | INTEROP-01, PRODUCT-01, ADOPTION-01 | Read-only `--doctor`, relocatable Linux remote-provider runtime packaging, Python and JavaScript/TypeScript host SDKs, tested local packages, reviewed MCP 2026-07-28 stdio bindings, native POSIX ACP session/stream/cancel/permission profile, accurate build/accounting documentation | ACP supplied-server lifecycle and editor interoperability, complete JSON Schema conformance and third-party MCP servers, HTTP/OAuth, signed packages and external user trials |

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
  Bubblewrap's private lifecycle channel must also confirm successful exec and
  the observed exit status; setup errors cannot count as failing candidate tests.
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
- Output contracts now travel explicitly through asmodel ABI 8. The remote
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
  and rejects double settlement. Schema-2 records correlate generation request
  spans with accounting and decode strict JSON frame by frame, avoiding the full
  history DOM. Invalid metadata or uncertain writes block further admission;
  failed replay cannot publish partial accounting. See [consumption](operations.md).
  Session-lifetime cost and monetary reconciliation
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

Acceptance-state replay now uses the same canonical byte check as approval
replay and a 256 KiB frame bound. Production WAL consumers all stream records;
the whole-document collector moved into test support. Regressions against
`7b3040f` reproduce accepted duplicate fields and premature torn-tail repair
before semantic rejection. Four cases also cover lossy strings, multiple states
in one frame, quota admission and the maximum 16-criterion definition over an
8 MiB/257-record history. See [state replay](state-replay.md).
The complete updated runs pass 55 restricted ASan/UBSan suites without LSan,
56 ordinary distribution suites and 51 non-threaded suites. Logs/JUnit are
`/tmp/asterism-state-final-{sanitize,native,nothreads}-tests.*`.

The approval checkpoint `7b3040f` passed clean reconstruction at
`/tmp/asterism-restricted-release-guqjcud8`: 54 Asngn, 31 Asper, 6 asmodel and
35 Astools suites. Its unsigned runtime archive at
`/tmp/asterism-runtime-approval-20260906/asterism-0.1.0-linux-x86_64.tar.gz`
contains 3,226,897 bytes with SHA-256
`f00aa909fa3a6b82cec5d4e102c15ba0e45b8873a11a0299a7b569f3bdab7e20`.
This artifact includes ACP and approval recovery, but predates acceptance replay.

Approval recovery now streams one canonical record per frame under a 2 MiB bound.
Checks run before torn-tail repair and before the recovered state is published.
Initial regressions against `6a737e8` reproduce accepted extra fields and multiple
transitions in one frame. A follow-up exposed xCDN's last-key replacement, so
canonical writer comparison also rejects identical duplicates, escaped NULs and
unwritten annotations. The negative-test fixture now joins its workers before
returning from a failed reproduction; an earlier negative-run ASan finding came
from that test cleanup path, not the replay runtime. Seven dedicated tests include
maximum escaped arguments and a 256-record history over 32 MiB. See
[approval recovery](approvals.md) for the exact contract.
The updated tree passes 54 restricted ASan/UBSan suites without LSan, 55 ordinary
distribution suites and 50 non-threaded suites. Their logs and JUnit reports are
`/tmp/asterism-approval-final-{sanitize,native,nothreads}-tests.*`.

The ACP checkpoint `6a737e8` passed clean reconstruction at
`/tmp/asterism-restricted-release-_prtarbh`: 53 Asngn, 31 Asper, 6 asmodel and
35 Astools suites. Its unsigned runtime archive is
`/tmp/asterism-runtime-acp-20260906/asterism-0.1.0-linux-x86_64.tar.gz`,
3,225,886 bytes, SHA-256
`2083bcae4405ec3fb6467eef342cdfb802c3d90900b527f3636b6139ec6c13ae`.
The adjacent receipt and JUnit/log record exact clean pins, relocated ACP startup,
doctor and packaged strict tools. This archive predates the approval replay fix.

The native ACP profile links the same runtime and shared asynchronous observer;
it does not add a model owner or expose approval mutation to MCP tool clients.
Actual runtime action events correlate review UUIDs with action UUIDs and journal
observations. Tests distinguish successful/failed processes from uncertain writes.
ACP suites exercise real stdio framing, UTF-8 output recovery, exact 64-bit IDs,
session limits, repeated/unknown permissions, stale workspace approval, cancel,
close, EOF, stalled output and separate live sessions. A relocated installation
also initializes the packaged host. The supported profile and its remaining
supplied-MCP/editor conformance gates are documented in [ACP](acp.md).
The complete restricted runs pass 53 ASan/UBSan executable suites (leak detection
disabled), 54 ordinary distribution suites and 49 non-threaded suites. After
caching the final answer length to avoid repeated full scans during output drain,
all three ACP suites pass again in both threaded builds and relocated distribution
passes again. Logs/JUnit are under `/tmp/asterism-acp-*-tests.*`; the final targeted
checks use `/tmp/asterism-acp-final-*-tests.*`. HTTP, LSan and the production
bubblewrap oracle remain excluded for the existing infrastructure reasons below.

The retrieval/fuzzing checkpoint `1b01e44` passed clean reconstruction at
`/tmp/asterism-restricted-release-bop9b65l`: 49 Asngn, 31 Asper, 6 asmodel and
35 Astools executable suites passed. This clean result predates ACP.

The `6974045` process/task checkpoint also passed clean reconstruction at
`/tmp/asterism-restricted-release-kexafidt`: 48 Asngn, 31 Asper, 6 asmodel and
35 Astools suites passed. Its clean unsigned runtime archive is
`/tmp/asterism-runtime-recovery-20260906/asterism-0.1.0-linux-x86_64.tar.gz`,
2,653,785 bytes, SHA-256
`f8082f25e2ff6b738e3b8a38f10c46fd981527fc0e283075843d58c3d4dca51e`.
The adjacent receipt records source pins, relocated installation, read-only doctor
and a real packaged strict-sandbox tool. This archive predates the following
parser/admission changes; it is not evidence for their packaged behavior.

Astools `3d5b810` adds optional real-library Clang/libFuzzer targets for JSON,
manifest/duration and MCP schema admission. The final bounded run completed
2,494,558 / 90,851 / 1,443,811 executions respectively without a finding; the
reviewed seeds, source changes, binary and log hashes are recorded. Both ordinary
and ASan/UBSan standalone builds pass 35 suites. Local leak detection is disabled;
remote CI and storage/process/concurrency fuzzing remain separate gates. A small
C99 call-site correction also makes the GNU string functions compile under
Clang 22/glibc without relaxing warnings. See [tool parser fuzzing](../../asterism-astools/docs/fuzzing.md).

Repository admission now keeps its resident cap while continuing bounded scans,
selecting by query and limiting each non-active file to eight chunks. Late files
and late Unicode ranges survive pressure from early unrelated files; empty
corpora issue no embedding calls. A min-heap bounds resident selection work and
64 MiB bounds captured content, separately from metadata and chunk counts.
Active hashes are rechecked, including non-code extensions and binary changes;
uncancelled traversal alone cannot hide missing source validation. Six dedicated
retrieval cases and the extended engine/embedding fixtures cover these contracts.
The late-file regression fails against `6974045`. The final implementation passes 49 restricted ASan/UBSan suites (without LSan),
50 ordinary distribution suites and 48 non-threaded suites.
This is a controlled recall fix, not measured task quality, semantic navigation,
incremental indexing or calibrated selection. See [admission bounds](workspace-tree.md#retrieval-evidence).


The last complete external-sandbox checkpoint was `7e62625`: 48/48 integrated
tests passed in native CPU and ASan/UBSan/LeakSanitizer builds, and all four
components passed from clean clones. No weights were loaded.

The subsequent ancestor-marker correction, request traces and consumption
correlation are built against the same pinned llama.cpp. asmodel now uses ABI 8;
the coordinated manifest and Asper's standalone pin identify the updated contract.
Local validation runs use
ASan/UBSan with leak detection disabled. The automatic permission reviewer timed
out twice when asked to run the current tests outside the sandbox. Current
LeakSanitizer, HTTP-wire/SDK live and bubblewrap acceptance checks therefore
remain open; do not inherit their earlier passes as validation of new changes.
The current restricted run passes 45 executables in both native CPU and ASan/UBSan
builds. Three HTTP live tests were excluded. The remaining oracle executable
passes 25 of its 26 cases; its production-sandbox case cannot create a netlink
socket under the outer sandbox. No acceptance check was weakened to hide that
infrastructure failure. There are now 49 integrated executables.
The additional six cases validate the private bubblewrap status channel, malformed
or missing terminal records, actual failed startup, descriptor inheritance and
I/O errors. Failed startup now retains its output/exit code but is explicitly
`infrastructure_error`; it previously appeared to be a failing candidate command.
This change has not yet passed successful production isolation outside the outer
sandbox. The earlier complete oracle run remains evidence for the earlier code.

Asper ABI 8 adds explicit source deferral, separate from source acknowledgement.
The integrated engine also passes 45 restricted ASan/UBSan executables against
this memory ABI. Its 31 standalone executable suites pass in ordinary,
ASan/UBSan (without leak detection) and
non-threaded builds. Eight new integration cases cross the actual C runtime,
Python operator CLI, checked export and MCP observation; later sources proceed
while the postponed event remains exact and unacknowledged. Decision removal
allows later curation when the transcript budget admits it. Corrupt or changed
source bindings fail store opening. See [offline deferral](../../asterism-asper/docs/curation-deferral.md).

Astools adds a reviewed MCP stdio adapter in `88fe2d0` and shared batch
admission in `aa8c418`, ABI 5 unchanged. It uses
per-request MCP 2026-07-28 metadata, local command bindings, input/output validation,
full error/incomplete payloads, bounded discovery and the existing sandboxed
supervisor. Its JSON Schema profile deliberately rejects unsupported keywords;
this is not full MCP conformance or a general ACP integration. All 35 standalone
executables pass in ordinary and ASan/UBSan builds (leak detection disabled),
and all 29 non-threaded executables pass. The new process/package tests cover
both stdio hops, Linux basic/strict isolation and synthetic host environment
grants. The updated integrated engine passes 45 restricted ASan/UBSan suites. See [MCP client scope](../../asterism-astools/docs/mcp-client.md).
The MCP milestone now also passes a clean four-component reconstruction at
`/tmp/asterism-restricted-release-8rveqo3n`: Asngn `155e3c5`, Asper `6cfb137`,
asmodel `f097e33`, Astools `aa8c418`; 45/31/6/35 executables passed respectively.
HTTP/LSan/bubblewrap checks remain excluded for the stated infrastructure reasons.

The Linux remote-provider distribution profile adds CMake installation and CPack
archives, with local clean-pin packaging and explicit unsigned build receipts.
A relocated installation and the extracted archive both pass read-only doctor,
manifest checking and real packaged strict-sandbox file reads. The distribution
build passes 47 restricted executable suites, including archive rejection tests.
This does not validate inference, other Linux distributions or publisher signing.
See [runtime distribution](distribution.md).

Durable task observations now distinguish interrupted admission, conversational
commit and a persisted terminal runtime outcome. The C API, MCP `agent_recover`
and both SDKs recover exact answers/partial output and action uncertainty after
handle release or server restart. Journal recovery streams frames; uncertain
writes block further appends. NO_THREADS confirmations require an immediate host
callback decision and fail explicitly when none arrives. Eight task-record cases
and four synchronous approval cases supplement the existing threaded checks.
The updated tree passes 48 restricted ASan/UBSan executables (leak detection off),
49 ordinary distribution executables and 47 non-threaded executables. Both SDK
archives pass isolated installation, live MCP calls and installed TypeScript
checks. These results do not establish resumable decoding or external-effect replay.
See [task recovery](task-recovery.md).

The complete clean-source packaging CLI also produced an unsigned archive from
Asngn `7ed4e7a`, Asper `6cfb137`, asmodel `f097e33` and Astools `aa8c418` at
`/tmp/asterism-runtime-local-20260906/asterism-0.1.0-linux-x86_64.tar.gz`.
Its 2,628,136 bytes have SHA-256
`dc011c39cec91de17d8dcfd4e1918641daef070964c250d8d3f4cfe4ea3d436a`;
the adjacent build receipt, JUnit and log record clean pins, relocation and
extracted-archive checks. That archive predates the task-recovery extension.

The task-recovery checkpoint has also passed clean reconstruction at
`/tmp/asterism-restricted-release-9a7y85y_`: Asngn `fe0e127`, Asper `6cfb137`,
asmodel `f097e33` and Astools `aa8c418`; 48/31/6/35 executable suites passed.

Astools `0174cfa` fixes the POSIX local timeout after early standard-pipe closure,
keeps spawn/capture/reaping under one local deadline, and reports `timed_out`
separately from exit status. One integer duration parser preserves milliseconds
in manifests, type validation, configuration and execution; period bounds exclude
Windows' infinite-wait sentinel. Shared output encoding retains NUL/invalid UTF-8
bytes as base64 with explicit byte counts in proc results and project steps.
Astools passes 35 ordinary, 35 ASan/UBSan (without LSan) and 29 non-threaded
executables; the updated integrated engine passes 48 restricted ASan/UBSan suites.
These are one-shot process contracts, not interactive process persistence or
new Windows enforcement claims. See [process results](../../asterism-astools/docs/process-results.md).

The preceding clean restricted checkpoint at
`/tmp/asterism-restricted-release-x_4_t5zu` verified Asngn `623ecad`, Asper
`6cfb137`, asmodel `f097e33` and the earlier Astools `228ec5c` with respectively
45, 31, 6 and 32 executables. It does not validate the new MCP adapter.

The shared runtime adds optional Clang/libFuzzer targets for strict JSON semantic
round trips and provider/embedding decoding, instrumenting the actual library.
A local seed-1 run completed 2,049,489 JSON and 305,386 provider executions, with
30 seconds requested per target and no finding. ASan/UBSan were active, leak
detection was disabled; this is bounded parser testing, not a security audit.
The CI jobs are defined but have not run remotely. See the [fuzzing record](../../asterism-asmodel/docs/fuzzing.md).

Four trace unit cases cover role/correlation metadata, omitted-item hashing,
framed boundaries, output constraints and invalid structures. Native integration
also correlates request/result spans and proves context rejection performs no
inference. Trace failures remain best-effort telemetry, not a task outcome.
The native integration additionally joins each dispatched generation to exactly
one durable reservation and settlement, and finds none for context rejection.
Six accounting cases cover unknown/cancelled usage, immutable correlation, changed
metadata, duplicate/NUL/unknown fields, arithmetic overflow, uncertain sync,
interleaved settlement across hash-table growth and failed replay publication.
Invalid complete frames cannot be joined across record boundaries or discarded
as an incomplete tail. The component replay comparison and its sanitizer limits
are documented with [raw measurements](operations.md#component-measurement).
Standalone Asper passes 30/30 with the updated runtime and ASan/UBSan; asmodel
passes 6/6 available restricted checks in ordinary and sanitizer builds. Its HTTP
check remains pending with the other external-sandbox validation.
Clean local clones at engine `c02b6f9`, Asper `48f6720`, asmodel `2d0e3db` and
astools `228ec5c` also build and pass the restricted release checks: 45 engine,
30 memory, 6 runtime and 32 tool executables, plus 25 selected oracle cases.
The HTTP target is explicitly built even though its execution is pending. This
is a clean-build checkpoint, not completion of the external validation gates.

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
- Nine Git metadata regressions and real SHA-1/SHA-256 worktree checks pass.
  Traversal references, symlinked metadata, special files and invalid OIDs cannot
  produce identity or a fingerprint. Common-store refs and private worktree refs
  remain distinct. The same probe against `22a8101` demonstrates the previous
  external-file read; see [snapshot boundaries](workspace-tree.md) for supported
  layouts and the remaining filesystem race and platform limits.
  A later sandbox regression also ensures an empty `.git` directory in an
  ancestor cannot be mistaken for the selected repository root.
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
  Oversized head events stop with `LIMIT` and remain pending. Explicit offline
  deferral can now postpone a reviewed event while later sources proceed;
  segmentation remains open. This is a contract regression, not model quality.
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
3. Add durable task resumption and controlled processes; extend the native ACP
   profile with supplied-server lifecycle and validate actual editor integration.
   [ACP v1 requires MCP stdio clients](https://agentclientprotocol.com/protocol/v1/session-setup).
   Integrate tool-client lifecycle and host permission boundaries before claiming
   ACP conformance; a text-only facade that ignores supplied servers is insufficient.
4. Run the real-model matrix with supplied configuration before admitting learned
   routing, reusable procedures, alternative patches or offline policy promotion.
