import assert from 'node:assert/strict';
import { Client } from '../javascript/src/index.mjs';

const client = await Client.start(JSON.parse(process.argv[2]));
try {
  const usage = await client.consumption();
  assert.equal(usage.lifetime.charged_tokens, 9007199254741115n);
  assert.equal(usage.lifetime.unsettled_tokens, 9007199254740993n);
  assert.equal(usage.lifetime.unknown_tokens, 100n);
  assert.equal(usage.lifetime.failed_calls, 1n);
  assert.equal(usage.lifetime.cancelled_calls, 1n);
  assert.equal(usage.today.charged_tokens, 0n);
} finally { await client.close(); }
