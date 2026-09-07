# Local ACP editor host

`asngn-acp` is a native C99 frontend over the same runtime, tool policy, task
journal and shared model manager as the TUI and MCP server. The initial profile
implements ACP v1 session turns on threaded POSIX systems. It is **not full ACP
conformance**: editor-supplied MCP servers are rejected explicitly; ACP requires
stdio MCP support. Preconfigured, reviewed ⁂ astools packages remain available.
No third-party editor interoperability or real-model quality run is claimed.

## Start

Threaded POSIX builds enable `ASNGN_BUILD_ACP` by default. Other builds leave it
off; requesting it without POSIX threads is a configuration error. The Linux
runtime archive includes the executable and this document.

```sh
./build/asngn-acp --workspace /absolute/repository \
  --root /absolute/engine-state --config /absolute/config.xcdn
```

Use this executable and argument list in the editor's custom agent setting.
Stdout carries only newline-delimited JSON-RPC; startup errors use stderr.
Model/tool configuration is the normal engine configuration. An explicitly
requested degraded startup permits protocol inspection with missing dependencies;
failed inference returns an error, never a successful coding result. The frontend
does not install servers, fetch weights or change the operator's action policy.

## Supported contract

| Operation | Behavior |
|---|---|
| `initialize` | Negotiates protocol 1 once; advertises text/resource links and `session/close` |
| `session/new` | Opens a fresh session at the configured canonical workspace; at most eight live sessions |
| `session/prompt` | One active prompt per session, asynchronous accepted answer chunks with a stable message ID |
| `session/request_permission` | Host request to the editor; only allow-once and reject-once decisions |
| `session/cancel` | Notification; cancels inference/tools and answers the original prompt with `cancelled` |
| `session/close` | Cancels outstanding work, completes its prompt response, then closes the session handle |

`session/new` requires an empty `mcpServers` array. `additionalDirectories` must
be absent or empty. Sessions are scoped to this stdio host; a guessed existing
store slug cannot attach a session. Image/audio/embedded-resource blocks, server
authentication, session load/resume/list and editor filesystem/terminal delegation
are not advertised. `resource_link` becomes a named reference in the prompt; it
does not itself open a file, fetch a URL or grant access outside the workspace.
Annotations and `_meta` values never grant capabilities.

## Permissions and observations

The editor channel invokes the public host confirmation API. The MCP tool channel
remains unable to approve itself. A review includes complete redacted arguments,
their original hash, package hash, workspace snapshot and turn ID. The engine
persists the decision and rechecks the package/snapshot before dispatch. An edited
workspace can invalidate an approved action. Unknown, late or repeated replies
cannot grant another action; an invalid decision denies and cancels its turn.
No persistent permission is inferred from a suggested option or remote annotation.

Permission tool-call IDs use the approval UUID. Actual dispatch and observation
events join that ID to the journal's action UUID. Unconfirmed read-only dispatches
use their action UUID directly. Tool statuses report execution, not task correctness;
`completed` does not certify an artifact. The ⁂ asterism `_meta` namespace records
`action_id`, `journaled` and the dispatch error. `journaled:false` means an external
result was observed but its journal write was not confirmed. Full observations
remain in the task journal rather than being copied into telemetry. Cache hits do
not manufacture dispatches. Telemetry remains best-effort; these live updates are
not a replacement for journal inspection after an interruption.

`end_turn` means the answer finished. Prompt response metadata under
`dev.asterism/asngn` carries `task_id`, the runtime `outcome` and `task_state`.
Without a host acceptance contract, task state remains `unconfirmed`. Other engine
failures retain their named outcome in JSON-RPC error data. This frontend does not
create acceptance criteria from a model's claim of success.

## Bounds and recovery

Input frames are limited to 1 MiB and strict UTF-8 JSON; duplicate keys fail parsing.
Request IDs accept signed 64-bit integers, null or NUL-free strings up to 128 bytes.
Prompt admission permits at most 64 blocks and 256 KiB of joined text. The writer
has a 4 MiB queue and a ten-second deadline without output progress. Partial input
frames never block task polling. Oversized frames, duplicate pending request IDs,
event queue overflow and inconsistent answer prefixes end the connection rather
than acknowledge an ambiguous result. The native event queue holds 64 records.

Output uses the shared observer's 256 events of up to 4096 bytes each. If an event
is truncated or its cursor is lost, incremental output pauses; the host validates
the already delivered prefix against the final answer with SHA-256 and sends only
the missing suffix in UTF-8 chunks of at most 16 KiB. It never replays model calls.
Completed answers are drained in bounded batches, with rotation between sessions.
EOF cancels all active lanes before joining them. Cancellation latency still
depends on backend/tool cancellation behavior; EOF does not erase persistent state.

Tests use real pipes, the production executable with unavailable weights, and a
separate driver linking the production host to scripted models and actual ⁂ astools
dispatch. They cover framing, output quotas/stalls, session bounds, Unicode prefix
recovery, permission replay, stale snapshots, cancellation, EOF and independent
session IDs. They test contracts, not model quality or third-party compatibility.

Protocol references checked on 2026-09-06:
[schema](https://agentclientprotocol.com/protocol/v1/schema),
[session setup](https://agentclientprotocol.com/protocol/v1/session-setup),
[transports](https://agentclientprotocol.com/protocol/v1/transports).
