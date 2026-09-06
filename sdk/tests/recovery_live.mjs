import assert from 'node:assert/strict';
import { Client, ToolError } from '../javascript/src/index.mjs';

const [command, record] = process.argv.slice(2, 4).map(JSON.parse);
const client = await Client.start(command);
try {
  const session = client.session('archive');
  assert.deepEqual(await session.recover(record.task_id), record);
  const pending = await session.recover(process.argv[4]);
  assert.equal(pending.state, 'interrupted');
  assert.equal(pending.action_uncertain, true);
  assert.equal(pending.last_action, 'edit.apply {patch: pending α}');
  assert.equal(pending.execution_resumed, false);
  assert.equal(pending.events_replayed, false);
  assert.ok(!('outcome' in pending));
  await assert.rejects(client.task(record.task_id).poll(), error => error instanceof ToolError && error.code === 'ASNGN_ERR_NOT_FOUND');
} finally { await client.close(); }
