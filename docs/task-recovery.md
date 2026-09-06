# Durable task observations

Save the session slug and UUID returned by `agent_submit`. After releasing the
live handle, reconnecting or restarting the server, `agent_recover` reads the
authoritative journal for that pair. The C API is `asngn_session_task_read`;
release its allocated result with `asngn_task_record_free`.

The session must be idle. Admission already writes and syncs `started` before
enqueueing the task. Completion now syncs a separate `finished` frame before
publishing the terminal event or waking task waiters, including cancellation
and model failure. A failed journal write blocks further writes until reopen.
Conversation rollback leaves the operation-consumption ledger intact.

| Record state | Meaning |
|---|---|
| `interrupted` | Admission is durable, but there is no conversational commit or terminal runtime receipt. Execution may never have started. |
| `turn_committed` | The conversation committed, but a final runtime receipt is absent. It is unsafe to infer how later finalization ended. |
| `finished` | A terminal runtime outcome is durable. Read `outcome`; this state does not imply that acceptance criteria passed. |

The record includes the original input, admission work revision, complete saved
answer or partial answer, last action UUID and intent, and its last observation.
`action_uncertain` means an action intent has no matching observed frame; even
an observed tool result may require external reconciliation. Recorded failure or
cancellation is terminal, rather than an interrupted turn in recovery counters;
unobserved actions still contribute to the uncertainty count.

`admitted_work_revision` is historical. MCP's `task_state`, `work_revision` and
criteria describe **current** acceptance evidence, refreshed against the current
workspace. A changed definition is reported as `superseded`. Current evidence may
have come from a later turn; it is not a reconstruction of proof at the old turn's
end. An absent acceptance contract remains `unconfirmed`.

```python
async with await Client.start(command) as client:
    record = await client.session("repair").recover(saved_task_id)
    print(record["state"], record.get("outcome"), record["action_uncertain"])
```

```javascript
const record = await client.session('repair').recover(savedTaskId);
console.log(record.state, record.outcome, record.action_uncertain);
```

`agent_recover` requires exactly `session` and `task_id`. Unknown UUIDs return
`ASNGN_ERR_NOT_FOUND`; busy or unrecovered sessions return `ASNGN_ERR_BUSY`.
MCP rejects a serialized record above 3 MiB with `ASNGN_ERR_LIMIT`, preserving
room for its outer JSON envelope; it never silently truncates evidence. The C API
can retrieve larger records within the existing 16 MiB frame/256 MiB journal
limits. SDKs validate lifecycle invariants before exposing the result.

Journal scans validate framing and checksums one frame at a time. Inspection
does not repair even an incomplete tail; reopening a session can repair that
tail according to the existing recovery rules. The new reader requires the
admission `work_revision` field and does not invent historical metadata for
older records. There is no task index or archived-event pagination yet.

Recovery never creates a live task handle, restores decoding, continues an
approval, reruns an inference or tool, or replays stream events. The response
explicitly contains `execution_resumed: false` and `events_replayed: false`.
`agent_poll`, cancel and release continue to address live handles only. Resume
with effect reconciliation and durable event cursors remains a separate milestone.

Tests cover actual worker success/failure/cancellation, release and reopen,
partial output, stale acceptance revisions, action uncertainty, read-only torn
tails, malformed terminal frames, output quotas, write failures, and process
exit before/after terminal sync. Both SDKs also exercise the actual stdio server
with deliberately missing weights, followed by restart and an interrupted-action
fixture. These are contract and process-crash checks, not real-model evaluation
or proof of power-loss durability.
