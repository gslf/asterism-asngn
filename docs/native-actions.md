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
invoked. Classification and the final response phase retain their configured
policies, including the optional response judge. This is an alternative action
protocol, not yet a measured reduction in latency or total inference cost.

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

Deterministic tests cover batch preflight, disabled capabilities, artifact gates,
discovery, result correlation and interruption. An HTTP integration test exercises
the complete MCP/Asngn/asmodel/Astools path for Chat Completions and Responses with
scripted local peers. These tests establish contract behavior, not real-model
quality, calibration or performance.
