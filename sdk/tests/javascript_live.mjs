import assert from 'node:assert/strict';
import { access } from 'node:fs/promises';
import { setTimeout as delay } from 'node:timers/promises';
import { Client, RequestTimeout, RpcError, ToolError } from '../javascript/src/index.mjs';

const command = JSON.parse(process.argv[2]);
const definition = { goal: 'Regression α', constraints: 'Keep original tests', criteria: [{
  id: 'regression', requirement: 'The test must run', command: 'test', adapter: 'cmake', path: '.', depends_on: 0,
}] };
let taskID;
let client = await Client.start(command);
try {
  const session = client.session('sdk');
  assert.equal((await session.approval()).status, 'none');
  assert.equal((await session.work()).task_state, 'unconfirmed');
  assert.equal((await session.defineWork(0, definition)).work_revision, 1);
  await assert.rejects(session.defineWork(0, definition), e => e instanceof ToolError && e.code === 'ASNGN_ERR_BUSY');
  await assert.rejects(client.callTool('session_approval', { session: 'sdk', allow: true }), e => e instanceof RpcError && e.code === -32602);
  const task = await session.submit('hello sdk-normal');
  taskID = task.id;
  const pages = [];
  for await (const page of task.updates({ timeout: 10000, interval: 10 })) pages.push(page);
  const result = pages.at(-1);
  assert.equal(result.outcome, 'ASNGN_OK');
  assert.equal(result.task_state, 'incomplete');
  assert.equal(result.answer, 'Asterism α 日本語 '.repeat(400));
  assert.ok(pages.some(page => page.events.some(event => event.truncated)));
  assert.deepEqual((await client.task(task.id).poll(result.next_cursor)).events, []);
  await session.invalidateWork(1);
  assert.equal((await task.poll()).task_state, 'superseded');
  assert.equal((await task.release()).done, true);
  await assert.rejects(task.poll(), e => e instanceof ToolError && e.code === 'ASNGN_ERR_NOT_FOUND');
  const slow = await client.session('slow').submit('hello sdk-slow');
  let started = false;
  for (let i = 0; i < 500; i++) {
    try { await access(process.argv[3]); started = true; break; }
    catch (error) { if (error.code !== 'ENOENT') throw error; }
    await delay(10);
  }
  assert.ok(started, 'HTTP request did not start');
  await assert.rejects(slow.wait({ timeout: 30, interval: 10 }), RequestTimeout);
  assert.equal((await slow.release()).done, false);
  await slow.cancel();
  assert.equal((await slow.wait({ timeout: 10000, interval: 10 })).outcome, 'ASNGN_ERR_CANCELLED');
  await slow.release();
} finally { await client.close(); }
client = await Client.start(command);
try {
  const state = await client.session('sdk').work();
  assert.equal(state.work_revision, 2);
  assert.equal(state.criteria[0].status, 'not_run');
  assert.equal((await client.session('sdk').approval()).status, 'none');
  await assert.rejects(client.task(taskID).poll(), e => e instanceof ToolError && e.code === 'ASNGN_ERR_NOT_FOUND');
} finally { await client.close(); }
console.log('JavaScript SDK: live lifecycle, truncated events, cancellation and store reopen passed');
