import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';
import { fileURLToPath } from 'node:url';
import { setTimeout as delay } from 'node:timers/promises';
import test from 'node:test';
import { Client, ProtocolError, RequestTimeout, RpcError, ToolError, TransportError } from '../javascript/src/index.mjs';
import { poll, taskRecord } from '../javascript/src/contract.mjs';

const peer = [process.env.ASTERISM_TEST_PYTHON || 'python3', '-B', fileURLToPath(new URL('peer.py', import.meta.url))];

test('durable outcomes do not imply replay or success', async () => {
  const { record, invalid_changes: changes } = JSON.parse(await readFile(new URL('task_records.json', import.meta.url), 'utf8'));
  assert.deepEqual(taskRecord(record, record.task_id), record);
  for (const change of changes) assert.throws(() => taskRecord({ ...record, ...change }, record.task_id), ProtocolError);
  for (const state of ['interrupted', 'turn_committed']) {
    const value = { ...record, state, turn_committed: state === 'turn_committed' };
    delete value.outcome;
    assert.deepEqual(taskRecord(value, value.task_id), value);
  }
});

test('parallel correlation, Unicode and structured errors', async () => {
  const client = await Client.start(peer);
  try {
    const values = await Promise.all(Array.from({ length: 32 }, (_, i) => client.callTool('echo', { i, text: 'α\n日本語' })));
    assert.deepEqual(values.map(v => v.i), Array.from({ length: 32 }, (_, i) => i));
    assert.equal(values[0].text, 'α\n日本語');
    await assert.rejects(client.callTool('rpc_error', {}), e => e instanceof RpcError && e.code === -32602 && e.data.field === 'x');
    await assert.rejects(client.callTool('denied', {}), e => e instanceof ToolError && e.code === 'ASNGN_ERR_DENIED');
    assert.deepEqual(await client.callTool('echo', {}), {});
  } finally { await client.close(); }
});

test('local deadline and abort retain correlation for late replies', async () => {
  const client = await Client.start(peer);
  try {
    await assert.rejects(client.callTool('late', {}, { timeout: 20 }), RequestTimeout);
    const controller = new AbortController();
    const waiting = client.callTool('late', {}, { signal: controller.signal });
    const rejected = assert.rejects(waiting, { name: 'AbortError' });
    await delay(20);
    controller.abort();
    await rejected;
    assert.deepEqual(await client.callTool('echo', { alive: true }), { alive: true });
    await delay(300);
    assert.deepEqual(await client.callTool('echo', {}), {});
    assert.equal(client.rpc.pending.size, 0);
    const unusual = new AbortController();
    const aborted = client.callTool('late', {}, { signal: unusual.signal });
    const observed = aborted.then(() => assert.fail('abort resolved'), reason => assert.equal(reason, null));
    unusual.abort(null);
    await observed;
  } finally { await client.close(); }
});

test('bounded pending queue, outgoing frames and idempotent close', async () => {
  const client = await Client.start(peer);
  try {
    await assert.rejects(client.callTool('echo', { text: 'x'.repeat(8 * 1024 * 1024) }), TransportError);
    const calls = Array.from({ length: 64 }, () => client.callTool('hold', {}));
    const settled = Promise.allSettled(calls);
    await assert.rejects(client.callTool('echo', {}), /budget/);
    await Promise.all([client.close(), client.close()]);
    assert.ok((await settled).every(v => v.status === 'rejected' && v.reason instanceof TransportError));
    assert.notEqual(client.rpc.process.exitCode ?? client.rpc.process.signalCode, null);
  } finally { await client.close(); }
});

test('malformed peers fail closed and release child resources', async () => {
  for (const mode of ['malformed', 'duplicate', 'badutf8', 'oversize', 'wrong_id', 'unterminated', 'exit']) {
    const client = await Client.start(peer);
    try { await assert.rejects(client.callTool(mode, {}), TransportError); }
    finally { await client.close(); }
    assert.notEqual(client.rpc.process.exitCode ?? client.rpc.process.signalCode, null, mode);
  }
});

test('startup failure is observable', async () => {
  await assert.rejects(Client.start([fileURLToPath(new URL('missing-server', import.meta.url))]), TransportError);
});

test('stderr is drained and bounded', async () => {
  const client = await Client.start(peer);
  try {
    await client.callTool('stderr', {});
    await delay(50);
    assert.ok(client.stderr.endsWith('α-end'));
    assert.ok(client.rpc.diagnostics.length <= 65536);
  } finally { await client.close(); }
});

test('descendant does not retain pipes', { skip: process.platform === 'win32' }, async () => {
  const client = await Client.start(peer);
  const { pid } = await client.callTool('orphan', {});
  await client.close();
  for (let i = 0; i < 100; i++) {
    try {
      process.kill(pid, 0);
      if (process.platform === 'linux' && ['Z', 'X'].includes((await readFile(`/proc/${pid}/stat`, 'utf8')).split(' ')[2])) return;
    } catch (error) {
      if (['ENOENT', 'ESRCH'].includes(error.code)) return;
      throw error;
    }
    await delay(10);
  }
  assert.fail('descendant remained alive after close');
});

test('cursor gaps cannot become complete task claims', () => {
  const packet = { task_id: 'task', done: true, cursor_gap: true, next_cursor: 301,
    events: [{ cursor: 300, kind: 0, text: 'α', truncated: true }], outcome: 'ASNGN_OK', answer: 'partial', task_state: 'incomplete' };
  assert.equal(poll(packet, 'task', 0).task_state, 'incomplete');
  for (const change of [{ cursor_gap: false }, { task_state: 'complete' }, { next_cursor: 302 }, { done: 1 }]) {
    assert.throws(() => poll({ ...packet, ...change }, 'task', 0), ProtocolError);
  }
});
