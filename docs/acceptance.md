# Persistent acceptance contracts

A turn can commit while work remains incomplete. Clients can define a bounded
acceptance contract with `asngn_session_work_define` or the MCP `session_work`
tool. Definitions belong to the host; the internal model has no operation that
can define criteria or submit proof. Without a contract, task state is
`unconfirmed`.

For example, the arguments below define a build followed by a regression suite:

```json
{
  "session": "repair",
  "mode": "define",
  "expected_revision": 0,
  "definition": {
    "goal": "Repair recursive configuration merging",
    "constraints": "Preserve the original acceptance suite.",
    "criteria": [
      {"id":"build", "requirement":"Sources compile", "command":"build",
       "path":".", "adapter":"cmake", "depends_on":0},
      {"id":"regression", "requirement":"Regression suite runs and passes",
       "command":"test", "path":".", "adapter":"cmake", "depends_on":1}
    ]
  }
}
```

`depends_on` is a bit set over earlier criterion positions; `1` means the first
criterion, `3` means the first two. Cycles, duplicate IDs, paths escaping the
workspace, oversized UTF-8 values and unsupported workflows are rejected. Up to
16 criteria are accepted. Public C strings have byte limits, including the final
NUL; the JSON schema's character limits do not replace these bounds.

To inspect the current state, use `{"session":"repair","mode":"get"}`.
Definition updates require the current revision and fail while a turn is
accepted. An unchanged criterion at the same position retains its proof only
when its dependencies and global constraints also remain unchanged. Editing the
goal's display title does not change acceptance semantics. Requirement text,
workflow, path and adapter are part of each criterion's definition.

The runtime records proof only after an actual closed `project` invocation and
the durable observed-action record. It matches the requested workflow and path,
checks receipt identity, adapter, typed status, exit code and test collection,
and requires equal workspace fingerprints before and after execution. Each
proof includes the action ID, workspace fingerprint and SHA-256 of the full raw
receipt retained in the turn journal. A read, cached tool result, absent command,
empty test suite or arbitrary process output cannot satisfy a criterion.

A passed prerequisite is required before its dependant can pass. A failed or
stale prerequisite blocks previous dependant proof; a later prerequisite success
does not resurrect it. Known mutations revoke proof durably before dispatch,
including mutations that later fail. State reads, model context and turn commits
check the current workspace. Criteria and constraints are mandatory prompt
content; a small context budget fails admission instead of silently dropping
them. Response cache reuse is disabled while a contract is active.

`succeeded` means every **declared check** passed on the observed snapshot.
This is not a claim that requirement prose has been proved, that test coverage is
complete, or that visible tests are protected. The host must choose meaningful
checks and protect its acceptance suite; the quality runner uses its own external
protected oracle. Environment and toolchain changes outside the workspace are
not automatically detected: use `mode: "invalidate"` with `expected_revision`
before reusing a session after such changes. File-level dependency invalidation,
external verifier adapters and non-executable acceptance rubrics remain future
work. Current invalidation conservatively covers the whole workspace.

Acceptance state uses its own versioned, checksummed `work.xcdn` WAL. It survives
cancelled/failed conversation turns and session reopen. A persistence error
requires reopen; the live process does not certify uncertain state. This log has
the same frame/log quotas as the action WAL and no implicit migration/compaction.

MCP polling reports `incomplete`, `succeeded`, `unconfirmed` or `unavailable`, plus
individual criteria. Each job captures its acceptance revision at admission. If
the host changes the definition, polling the older job reports `superseded`.
Release all retained job handles before deleting their session. Event cursor
retention is still process-local even though the acceptance contract persists.
