import { setTimeout as delay } from 'node:timers/promises';
import { Transport } from './transport.mjs';
import { payload, poll, states } from './contract.mjs';
import { integer, positive } from './json.mjs';
import { ProtocolError, RequestTimeout } from './errors.mjs';

export class Client {
  constructor(transport) { this.rpc = transport; this.serverInfo = {}; }

  static async start(command, options = {}) {
    const client = new Client(new Transport(command, options));
    try {
      const reply = await client.rpc.request('initialize', {
        protocolVersion: '2025-06-18', capabilities: {},
        clientInfo: { name: 'asterism-javascript', version: '0.1.0' },
      }, options);
      if (reply?.protocolVersion !== '2025-06-18' || reply.serverInfo?.name !== 'asngn-mcp' || typeof reply.serverInfo?.version !== 'string' ||
          reply.capabilities?.experimental?.['dev.asterism/asngn']?.contractVersion !== 1) {
        throw new ProtocolError('unsupported Asterism contract; version 1 required');
      }
      client.serverInfo = reply.serverInfo;
      client.rpc.initialized();
      return client;
    } catch (error) {
      await client.close();
      throw error;
    }
  }

  get stderr() { return this.rpc.stderr; }
  async close() { await this.rpc.close(); }
  async [Symbol.asyncDispose]() { await this.close(); }
  async callTool(name, args, options) {
    return payload(await this.rpc.request('tools/call', { name, arguments: args }, options));
  }
  session(slug = 'main') {
    if (typeof slug !== 'string' || !slug || slug.includes('\0')) throw new TypeError('invalid session slug');
    return new Session(this, slug);
  }
  task(id) {
    if (typeof id !== 'string' || !id || id.includes('\0')) throw new TypeError('invalid task ID');
    return new Task(this, id);
  }
}

export class Session {
  constructor(client, slug) { this.client = client; this.slug = slug; }
  async submit(message, options) {
    const result = await this.client.callTool('agent_submit', { session: this.slug, message }, options);
    if (typeof result.task_id !== 'string' || !result.task_id) throw new ProtocolError('submit returned no task handle');
    return this.client.task(result.task_id);
  }
  async work() { return this.readWork({ mode: 'get' }); }
  async defineWork(expectedRevision, definition) {
    return this.readWork({ mode: 'define', expected_revision: expectedRevision, definition });
  }
  async invalidateWork(expectedRevision) { return this.readWork({ mode: 'invalidate', expected_revision: expectedRevision }); }
  async readWork(args) {
    const value = await this.client.callTool('session_work', { session: this.slug, ...args });
    if (!states.has(value.task_state)) throw new ProtocolError('invalid acceptance state');
    return value;
  }
  async approval() {
    const value = await this.client.callTool('session_approval', { session: this.slug });
    if (!['none', 'pending', 'approved', 'denied', 'consumed', 'invalidated', 'interrupted'].includes(value.status)) {
      throw new ProtocolError('invalid approval state');
    }
    return value;
  }
}

export class Task {
  constructor(client, id) { this.client = client; this.id = id; }
  async poll(cursor = 0, options) {
    if (!integer(cursor)) throw new TypeError('cursor must be a nonnegative safe integer');
    return poll(await this.client.callTool('agent_poll', { task_id: this.id, cursor }, options), this.id, cursor);
  }
  async cancel() { return poll(await this.client.callTool('agent_cancel', { task_id: this.id }), this.id, 0); }
  async release() { return poll(await this.client.callTool('agent_release', { task_id: this.id }), this.id, 0); }
  async *updates({ cursor = 0, interval = 100, timeout, signal } = {}) {
    positive(interval, 'interval');
    const deadline = timeout === undefined ? Infinity : performance.now() + positive(timeout, 'timeout');
    while (true) {
      signal?.throwIfAborted();
      const remaining = deadline - performance.now();
      if (remaining <= 0) throw new RequestTimeout('local task wait expired; task remains available');
      const page = await this.poll(cursor, { timeout: Math.min(this.client.rpc.timeout, remaining), signal });
      yield page;
      if (page.done) return;
      cursor = page.next_cursor;
      await delay(Math.max(0, Math.min(interval, deadline - performance.now())), undefined, { signal });
    }
  }
  async wait(options) {
    for await (const page of this.updates(options)) if (page.done) return page;
    throw new ProtocolError('task stream ended without an outcome');
  }
}
