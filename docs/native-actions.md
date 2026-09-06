# Native action protocol

The default controller uses constrained action objects. Operators can select native
function proposals for the configured generator:

```xcdn
routing: { native_actions: true }
integration: { astools: { tool_limit: 16, tool_schema_bytes: 24000 } }
models: { sampling: { decide: { max_tokens: 2048 } } }
```

This requires a backend that implements the asmodel native-tool contract. The
embedded adapter currently returns `ASNGN_ERR_UNSUPPORTED`. A provider label alone
does not certify a server/model/template combination. Test it before deployment.
No automatic fallback silently changes protocols after a failed native request.

The generator proposes selected tools directly, without a mandatory planner or
THINK action. File payloads are supplied inline; the separate draft model is not
invoked. Classification and response validation retain their configured policies,
including the optional response judge. A successful native text response can be
reused after validation, without a separate response-generation request. This is
not yet a measured improvement in real-model latency, cost or task success.

## Contract and execution

Tool names are stable provider-safe hashes of the exact reference, command and
package content. Descriptions include the public command label. Native schemas,
preflight and invocation use the same immutable Astools selection. Names change
when package identity changes; they cannot acquire authority from descriptions.
Five slots are reserved for runtime controls, so native selection supports at most
59 tools. A larger selection fails explicitly instead of dropping arbitrary tools.

Runtime functions provide discovery, memory recall, blob expansion, clarification
and transition to the response phase. Functions are absent when their capability
is unavailable. Their small JSON argument contracts are validated by the runtime;
Astools validates tool arguments and current permissions. `asterism_finish` takes
an empty object and does not assert task success. The artifact gate still applies.

When that gate allows reporting, tool choice is `auto`: a request can return calls
or a final text proposal. While a required artifact is missing, tool choice remains
`required`, and the runtime independently rejects a transition without the artifact.
Text accompanying tool calls is discarded. Empty/invalid text and failed or partial
generations cannot become user responses.

Final proposals are buffered until the response phase. They pass the same hard
tool-syntax gate and optional reviewer as generated responses; rejected text never
streams to the user. Denied actions, continuations, forced endings and artifacts
without a current verification receipt retain the dedicated response pass. Receipt
freshness is checked immediately before reuse, independently of the router's task
label. A text proposal does not certify task success or replace the acceptance graph.

The entire proposed batch is validated before its first effect. Multiple proposals
must all name read-only, non-destructive tools and fit the remaining action/tool
budget. They execute in order. These annotations narrow selection; host permissions
and sandbox enforcement remain authoritative. Batches are not atomic, and later
calls can fail after earlier ones executed. Dependent operations belong in later
requests. Mutations and runtime controls require individual proposals.

Both controllers use the same phase, cancellation, deadline, confirmation,
repetition, cache, action journal and verification checks. Identity and permission
checks run again inside invocation, including after queue waits. Invalid native
batches terminate with a precise protocol error before execution. A failed,
cancelled, limited or incomplete generation cannot dispatch its partial proposals.

## Context and limits

Initial repository and conversation context is compiled once. Current-turn
assistant calls and tool results retain roles and correlation IDs through asmodel;
the current acceptance and verification state is supplied separately on each
request. Earlier Asper history is still compiled text. Historical arguments and
observations follow session redaction policy. A redaction that breaks argument
JSON stops the request rather than sending the original sensitive text.

The in-memory transcript retains at most 64 observations and 1 MiB. Large working
items retain excerpts from both ends with explicit omission notices. These excerpts
are not verifier receipts; the existing evidence/journal pipeline is unchanged.
Reaching transcript or model context limits returns an explicit context error.
There is no silent role flattening, history eviction or durable native resume.
Native schemas count toward admission. Zone telemetry remains diagnostic token
accounting; per-operation usage receipts distinguish known and unknown consumption.

The prompt states both the action-generation ceiling and the final-response ceiling
(the smaller of the action and detail caps). A proposal that reaches its response
cap uses the dedicated response pass, which can rebuild context for that budget.
Native generation is charged once to its action-phase operation and `gt_decision`;
reuse adds no `gt_answer` charge. A reviewer or replacement response retains its own
operation receipt. The native path does not promise real-time output streaming.

Deterministic tests cover batch preflight, disabled capabilities, artifact gates,
discovery, result correlation, interruption, Unicode final text, response caps,
review rejection, stale verification, withheld output and single-charge reuse.
An HTTP integration test exercises
the complete MCP/Asngn/asmodel/Astools path for Chat Completions and Responses with
scripted local peers. In both wire formats, the explicit-finish baseline makes
four requests (classification, tool call, finish, response); the same scripted
answer as native text needs three (classification, tool call, final response).
This isolates one avoided request. It does not measure model quality, calibration,
real-server latency or financial cost.
