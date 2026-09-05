# asngn — Architecture and Design

## 1. Why this project exists

A small language model can write useful code, but it is unreliable when one
prompt asks it to understand a project, remember a long session, choose tools,
produce valid calls, inspect outcomes, manage safety and compose a final answer
at the same time. More prompt text does not solve that problem; it often makes
the model less focused and consumes the context it needs for the task.

asngn is an outcome-gated agent harness. It decomposes a turn into narrow model
passes and deterministic operations, then requires evidence before accepting a
claim of success.

Its central idea is:

> Let the model reason about the task. Move memory, syntax, validation, safety,
> resource control and success checks into systems that do not have to guess.

The harness is designed primarily for local small language models, while the
same contracts support larger and remote models.

## 2. The Asterism system

asngn is the orchestrator of four independent projects:

- **asmodel** owns model providers, shared residency, tokenization and inference.
- **Asper** owns exact events, semantic memory, checkpoints and large objects.
- **astools** owns typed tool contracts, permissions and supervised execution.
- **asngn** owns turn state, routing, action policy, guards, judging and clients.

The boundaries matter. asngn does not implement its own durable memory, provider
protocol or shell abstraction. This keeps each source of truth singular and
makes failures attributable to the correct layer.

## 3. The turn pipeline

A submitted turn becomes an asynchronous task. Its main path is:

1. **Ingest:** validate and normalize the user input, then append it to Asper.
2. **Context:** ask Asper for a bounded view using the selected model's tokenizer.
3. **Cache probe:** look for safe reusable work without bypassing outcome rules.
4. **Route:** classify the task and select direct or planned execution and a
   suitable model tier.
5. **Decide:** request exactly one schema-constrained next action.
6. **Act:** execute a tool, recall memory, reopen stored data, think, clarify or
   prepare to answer.
7. **Observe:** digest the real result, update evidence and run loop guards.
8. **Repeat:** continue decision and action steps until the outcome is supported.
9. **Draft:** generate the answer or artifact under its own explicit budget.
10. **Judge:** when enabled, check the response against task and evidence.
11. **Commit:** persist assistant output, checkpoint, ledger and telemetry.

Each model pass has one job and a contract small enough to validate. Deterministic
code carries state from one phase to the next.

## 4. Decision protocol

The decision pass emits one object with one action:

```text
{action, why, input, success, fallback}
```

Supported actions are `call`, `recall`, `open`, `think`, `clarify` and `answer`.
The exact schema is grammar-constrained through asmodel. The pass is short,
reasoning is disabled, and the output ceiling is explicit because its purpose is
selection, not prose generation.

This is an important token policy: a decision that cannot fit the bounded schema
is a failed decision, not a reason to regenerate a longer explanation. A length
stop, malformed object, provider error or timeout is returned once. The engine
does not repeat the whole decision and spend the same prompt tokens again.

The `success` field states what observable result would justify completion. The
loop compares that condition with tool and memory evidence; an `answer` action is
not automatically proof that the task succeeded.

## 5. Action and evidence

`call` selects a typed astools command. astools validates its arguments and
permissions before execution and returns a structured result. asngn records the
invocation, exit status, diagnostics and changed-world signal.

`recall` asks Asper for a focused memory answer. `open` reads a range from a
previously stored exact object. `think` allows one bounded private reasoning
step. `clarify` stops for information that cannot safely be inferred. `answer`
enters response production only when the current outcome policy permits it.

Evidence is always external to model confidence. A build is successful because
the build tool succeeded; a file was changed because the applicable patch exists;
a search was completed because the search result is present. The harness does
not accept fluent prose as a substitute for observable state.

## 6. Drafting and lossless continuation

Long-form output and code artifacts use a separate draft phase. The draft is
private working state, not a prematurely published answer.

If generation reaches its output-token ceiling, asmodel returns the partial
bytes instead of discarding them. asngn then:

1. preserves the exact partial draft as an Asper object and checkpoint;
2. records its full byte count and SHA-256;
3. supplies an exact UTF-8-safe suffix as continuation context;
4. requests only the continuation;
5. removes any repeated overlap at the join;
6. repeats until the model stops normally, the turn budget is exhausted or the
   user cancels.

The already generated prefix is never paid for a second time. Exact hashing and
overlap handling prevent a continuation from silently replacing or duplicating
the artifact.

A zero generation deadline means no wall-clock cutoff. Local inference may take
as long as it needs. Cancellation remains live through the task, model provider
and terminal UI; pressing Escape or invoking the C cancellation API terminates
the active generation and clears drafting state.

## 7. Memory and context ownership

All durable memory belongs to Asper. For every model pass, asngn supplies the
scope, query, base instructions and zone budgets to
`asper_context_materialize`. Asper returns:

- semantic identity, user context and active-project memory;
- the current working checkpoint;
- selected pinned and recent exact events.

Selection uses the tokenizer of the model that will consume the prompt. asngn
validates the final assembled context once more before inference.

There is no independent folding summary in asngn. It stores no alternate durable
transcript, compressed-history file or private large-result archive. If Asper is
disabled, the engine is explicitly ephemeral; it does not create a weaker hidden
memory system with different semantics.

This separation makes context exhaustion recoverable. Old material can leave a
particular prompt while exact events, checkpoints and objects remain available
for later retrieval and continuation.

## 8. Large tool results

A compiler, search or file tool can return more text than the next model call can
use. asngn digests such a result into a short view and stores the complete bytes
as a content-addressed Asper object.

The model receives the digest plus a stable handle such as `OPEN B1`. It can
request an exact range only when the missing detail becomes relevant. This is
progressive disclosure for evidence: the full result is retained, but prompt
space is spent on the part currently needed.

## 9. Model routing

The model pool assigns explicit roles such as router, planner, generator,
compressor and embedder. Entries may use embedded GGUF inference or explicit
remote profiles for llama.cpp server, LM Studio, vLLM and generic endpoints.
asmodel hides their protocol differences while exposing their actual
capabilities.

Routing is task-aware:

- deterministic heuristics handle cases that do not require inference;
- a small router or planner handles narrow classification and decision passes;
- the generator tier handles coding, complex reasoning and final artifacts;
- escalation occurs when the current tier cannot satisfy the outcome contract.

Cheap-first routing is an optimization, not a quality override. Coding work is
not served from an answer cache or forced through a weak tier merely to improve
a token metric. Route quality is evaluated against task outcome.

## 10. Caches

Two caches remove repeated work with different safety rules.

### 10.1 Semantic answer cache

Embedding similarity can surface prior answers for compatible, low-risk turns.
An answer may be adapted by a light pass when safe. Entries connected to tool
activity are not replayed as final answers; they can only become plan hints.

A world-epoch counter advances after destructive tool activity, preventing stale
results from crossing a changed workspace state. Coding profiles favor fresh
execution and outcome verification over answer reuse.

### 10.2 Exact tool cache

Repeated read-only invocations can reuse a result under an exact key. Mutating
activity invalidates this cache through the same world-state boundary. A cache
hit preserves the original structured evidence and avoids both process work and
the tokens needed to interpret duplicate output.

Provider-side prompt/KV caching is separate and handled by asmodel.

## 11. Guards and stopping rules

Guards make the action loop finite and evidence-driven. They activate only when
their measured condition occurs:

- **step, tool, think and recall caps** stop unbounded resource use;
- **identical-call guard** rejects repeating the same invocation without new
  evidence;
- **oscillation guard** detects alternating actions that do not advance state;
- **stall guard** detects steps that produce no useful progress;
- **futility guard** stops a fallback chain whose outcomes keep failing;
- **outcome guard** prevents a success claim without the required evidence;
- **context and output guards** prevent a request from exceeding model limits;
- **cancellation guard** stops promptly when the user asks.

These are not inference wall-clock limits. By default, time does not determine
whether a slow model is allowed to finish. Token ceilings, explicit resource
caps, evidence and user cancellation define the boundary.

When a guard fires, the turn ends with a specific diagnostic and preserves its
partial work. A guard does not silently launch a fresh model attempt.

## 12. Safety pipeline

Safety is layered around the model:

1. Input validation rejects invalid encoding and impossible request state.
2. The decision schema restricts the action language.
3. astools validates arguments, canonicalizes paths and enforces grants.
4. Destructive operations can require explicit human confirmation.
5. Secret redaction prevents known credentials from entering prompts or output.
6. Loop guards limit repeated or non-progressing behavior.
7. Output validation and the optional judge compare claims with evidence.

Tool output is treated as untrusted data, not as a new system instruction. The
model may reason about it, but it cannot use text inside a file or command result
to bypass host policy.

Each session persists a usage mode and a separate security profile. `chat`
forces direct conversation over transcript and retrieved memory without tools;
`coding` enables repository work; `automate` favors end-to-end execution and
verification. The explicit profiles are `chat`, `coding-readonly`,
`coding-sandboxed`, and `automation-ci`. Read-only policy blocks mutations;
CI permits non-destructive sandboxed actions without an interactive prompt;
an explicit deny policy takes precedence, and destructive actions still follow
confirmation policy. Missing authorization is returned as a visible,
non-terminal notice so the user can grant it or select a different profile
between turns.

## 13. Token economy

The engine optimizes useful work per token rather than minimizing tokens at any
cost.

- **Narrow passes:** classification, decisions and judging emit small constrained
  structures instead of essays.
- **Role routing:** capable expensive models are reserved for tasks that need
  them.
- **Zoned context:** instructions, memory, checkpoint, evidence and response
  space receive explicit budgets.
- **Consuming tokenizer:** budgets reflect the actual selected model.
- **Semantic retrieval:** relevant memory replaces full-history replay.
- **Checkpoints:** current state replaces repeated reconstruction of old work.
- **Progressive evidence:** large results become short digests plus exact reopen
  handles.
- **Prompt/KV reuse:** stable prefixes are reused by asmodel where supported.
- **Safe caches:** unchanged answers and read-only tool results avoid duplicate
  work without crossing mutation boundaries.
- **Continuation:** partial drafts continue from the valid prefix instead of
  restarting.
- **No implicit retry:** failures remain visible and are not multiplied.
- **Detail budgets:** terse, normal and rich responses receive explicit output
  ceilings appropriate to the user's request.

The non-negotiable rule is that an optimization may remove duplicated context or
computation, but may not remove required evidence, weaken a constraint or claim
success earlier. Token savings are recorded alongside task-quality signals, not
used as a substitute for them.

## 14. How the harness makes SLMs more capable

The harness compensates for structural limits rather than pretending they do not
exist.

### Limited context

Asper supplies durable, query-directed memory and exact reopenable sources. The
model sees a focused working set instead of a growing transcript.

### Fragile structured output

Grammar-constrained decisions and manifest-generated tool schemas remove invalid
syntax from decoding. Deterministic parsers still validate the result.

### Weak long-horizon planning

The model chooses one action at a time. asngn preserves state, checks progress
and supplies the next bounded problem.

### Hallucinated success

Outcome gates require tool results, applicable changes or other concrete
evidence. An optional judge inspects the final claim against that evidence.

### Limited tool knowledge

astools provides a compact catalog, typed arguments, examples and semantic
software operations. The model need not memorize platform-specific shell usage.

### Limited compute

Role routing, shared model residency, KV reuse, safe caching and continuation
spend compute on new reasoning rather than duplicated setup.

The resulting capability belongs to the complete system: model plus memory,
tools, constraints, evidence and orchestration.

## 15. Concurrency and cancellation

The public API submits work as tasks. Worker threads run turns without blocking
the host UI, while each session serializes state changes that must remain ordered.
Independent sessions can progress concurrently subject to model and tool
resource limits.

Cancellation is cooperative across layers but observable as one task result. It
reaches active asmodel generation and astools execution, preserves already
committed source events and partial artifacts, and returns the session to an idle
state. UI status is derived from task lifecycle rather than a detached timer.

## 16. Telemetry and quality accounting

Each turn writes a ledger attributing tokens to model role, tier, phase and
context zone. It records cache savings, tool activity, guard outcomes, latency
and user feedback. QpT, quality per token, is a diagnostic signal only.

For coding work, quality is grounded in stronger measures: valid tool calls,
applicable patches, successful builds and tests, absence of regressions and
faithful final reporting. A lower token count is not an improvement if those
signals deteriorate.

Telemetry uses monotonic clocks for durations and explicit event records for
postmortem inspection. It must make hidden retries and unexplained token growth
detectable; the architecture forbids the former and accounts for the latter.

## 17. Degraded operation

Subsystem failure is explicit:

- without Asper, sessions are ephemeral and durable recall is unavailable;
- without embeddings, semantic memory and cache operations degrade while exact
  source storage continues;
- without astools, the engine can converse but cannot claim tool-backed work;
- without a required provider capability, the affected model request fails
  before inference;
- without a judge, outcome gates and deterministic validation still apply.

The engine does not silently substitute a lower-integrity behavior under the
same success label.

## 18. Fundamental invariants

The implementation must preserve these rules:

1. Each model pass has one narrow, validated responsibility.
2. Durable memory, compaction, checkpoints and exact objects belong to Asper.
3. Model execution and provider compatibility belong to asmodel.
4. Tool syntax, permission and process policy belong to astools.
5. A claim of success requires observable evidence.
6. Required model constraints are never silently weakened.
7. Token-limit output is preserved and continued, never automatically retried.
8. A zero inference deadline is unbounded; the user can always cancel.
9. Mutating tool activity invalidates state-sensitive reuse.
10. Token optimization may remove duplication, never necessary quality.
11. Partial work and diagnostics survive guard or cancellation exits.
12. The ledger accounts for the operation that actually ran.

## 19. Public surfaces

`include/asngn.h` is the authoritative C99 host API. It covers engine and
workspace lifecycle, persistent modes and profiles, asynchronous turns, rich
output/reasoning/notice streams,
cancellation and confirmation, transcripts, feedback, projects, tools, recall,
exports, telemetry, model information and statistics.

The terminal client uses the same task lifecycle and exposes live streaming,
confirmation and cancellation. `asngn-mcp` exposes engine sessions to MCP
clients. Neither client has a separate orchestration policy.

Build instructions, model downloads and configuration examples belong in the
README and `examples/`. Event field references belong in `docs/telemetry.md`.
This document defines the architecture, its quality policy and the invariants
that implementation changes must preserve.
