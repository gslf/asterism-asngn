# Asterism JavaScript / TypeScript SDK

ESM host client for the local `asngn-mcp` server, Asterism wire contract 1.
Requires Node.js 22+. TypeScript declarations are included; no runtime dependencies.

```javascript
import { Client } from '@asterism/sdk';

const client = await Client.start(['asngn-mcp', '--root', '/absolute/store']);
try {
  const task = await client.session('example').submit('Hello');
  const result = await task.wait({ timeout: 120_000 });
  console.log(result.outcome, result.task_state, result.answer);
  await task.release();
} finally {
  await client.close();
}
```

Durations are milliseconds. `done` and `ASNGN_OK` do not certify task success;
inspect `task_state`. A local timeout/AbortSignal does not cancel the engine task;
call `task.cancel()`, poll to completion, then release. `task.updates()` yields
cursor pages including event gaps/truncation. Handles belong to the live server
and do not survive its restart. Save the session slug and UUID:
`session.recover(taskId)` reads durable outcomes and action uncertainty after
release or restart, without resuming execution. Approval inspection is read-only.

`await client.consumption()` reads lifetime/day inference accounting for the
whole engine store, including failures and shared memory calls. Counters are
`bigint`; use `.toString()` when serializing them. Known tokens, unknown usage and
unsettled reservations are separate; charges are not a monetary bill or session total.

See `sdk/README.md` in the source repository for the full lifecycle and limits.
