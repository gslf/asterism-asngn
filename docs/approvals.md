# Durable action approvals

Interactive confirmation reviews the effective arguments after any artifact
drafting. The request binds their SHA-256, the exact tool reference and package
content, session workspace, security profile and workspace snapshot. Review text
is complete and redacted; requests beyond the 256 KiB review bound fail explicitly.
The TUI scrolls the complete arguments with arrows, Page Up/Down and Home/End.
It cannot approve from a truncated telemetry preview if full details are unavailable.

The session's checked `approvals.xcdn` WAL records pending, approved/denied and
consumed/invalidated/interrupted transitions. The existing single-writer store
lock and framed checksums apply. An approval is durable before its worker wakes;
a failed write prevents execution and poisons the session until recovery. Later
transitions cannot rewrite reviewed fields. A conflicting duplicate decision
fails; an identical retry while the wakeup slot remains active is idempotent.

After approval, the engine rechecks the workspace snapshot and selected package.
A change while the user was reviewing invalidates that request. Consuming the
approval is durable before dispatch, and the action journal carries its ID.
Consumed means authorization was used, not that the tool ran or succeeded;
consult the action journal and verifier receipt for those outcomes. The ordinary
Astools identity/policy checks and edit preconditions still apply after queues.
The snapshot check and external process execution are not an atomic transaction;
concurrent writers still require edit hashes or isolated workspaces.

An explicit session grant is broader than approval of one request: it covers the
same command, exact package, workspace and security profile until that process's
session closes. Different arguments are allowed within host policy. Changing the
package or profile requires a new decision. Session grants do not persist across
restart. Operator `allow`/`deny` policies and permitted CI actions remain explicit
policy grants; they do not manufacture interactive approval receipts.

Embedded clients use `asngn_approval_get` to inspect the latest durable record and
`asngn_confirm` to answer an active request. `session_approval` exposes read-only
inspection over MCP, including complete redacted arguments. MCP tool clients do
not gain an approval mutation endpoint. Keep approval authority in the trusted host.

A client reconnecting to a running engine can rediscover the active request by
session and ID. Reopening the store marks pending or approved-but-unconsumed
records interrupted. It never replays the action. Durable task resumption and
reattaching an interrupted action to a new job remain separate work; this milestone
does not claim restart-safe execution or exactly-once external effects.

Tests exercise expanded file payloads, ownership of review copies, immutable
fields, changed snapshots and packages, rejected decision writes, reopen without
replay, read-only MCP inspection and scrolling to the end of long Unicode input.
