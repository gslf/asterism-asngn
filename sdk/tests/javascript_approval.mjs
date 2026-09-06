import assert from 'node:assert/strict';
import { createHash } from 'node:crypto';
import { setTimeout as delay } from 'node:timers/promises';
import { Client, RpcError } from '../javascript/src/index.mjs';

const command = JSON.parse(process.argv[2]);
let client = await Client.start(command), saved;
try {
  const session = client.session('review');
  const task = await session.submit('Run wire.mut');
  for (let i = 0; i < 500; i++) {
    saved = await session.approval();
    if (saved.status === 'pending') break;
    await delay(10);
  }
  assert.equal(saved.status, 'pending');
  assert.equal(JSON.parse(saved.arguments).msg, 'Review α 日本語 '.repeat(400));
  assert.equal(createHash('sha256').update(saved.arguments).digest('hex'), saved.arguments_sha256);
  assert.ok(Buffer.byteLength(saved.arguments) > 4096);
  assert.equal(saved.arguments_sha256.length, 64);
  assert.equal(saved.tool_ref, 'wire@1.0.0');
  assert.equal((await task.poll()).done, false);
  await assert.rejects(client.callTool('session_approval', { session: 'review', allow: true }), e => e instanceof RpcError && e.code === -32602);
  assert.equal((await session.approval()).status, 'pending');
  await task.cancel();
  assert.equal((await task.wait({ timeout: 10000 })).outcome, 'ASNGN_ERR_CANCELLED');
  assert.equal((await session.approval()).status, 'interrupted');
  await task.release();
} finally { await client.close(); }
client = await Client.start(command);
try {
  const value = await client.session('review').approval();
  assert.equal(value.status, 'interrupted');
  assert.equal(value.arguments, saved.arguments);
} finally { await client.close(); }
console.log('JavaScript SDK: complete pending approval, denied escalation and durable interruption passed');
