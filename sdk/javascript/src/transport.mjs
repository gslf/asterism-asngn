import { spawn } from 'node:child_process';
import { setTimeout as delay } from 'node:timers/promises';
import { decode, encode, integer, object, positive } from './json.mjs';
import { ProtocolError, RequestTimeout, RpcError, TransportError } from './errors.mjs';

const FRAME_BYTES = 8 * 1024 * 1024;

export class Transport {
  constructor(command, { timeout = 10000, cwd, env } = {}) {
    this.timeout = positive(timeout, 'timeout');
    if (!Array.isArray(command) || !command.length) throw new TypeError('command must be a nonempty argv array');
    this.process = spawn(command[0], command.slice(1), {
      cwd, env, shell: false, detached: process.platform !== 'win32', stdio: ['pipe', 'pipe', 'pipe'],
    });
    this.next = 0;
    this.bytes = 0;
    this.pending = new Map();
    this.failure = null;
    this.closing = null;
    this.diagnostics = Buffer.alloc(0);
    this.buffer = Buffer.allocUnsafe(8192);
    this.length = 0;
    this.exited = new Promise(resolve => {
      this.process.once('exit', resolve);
      this.process.once('error', resolve);
    });
    this.process.on('error', error => this.fail(new TransportError(error.message)));
    this.process.stdin.on('error', error => this.fail(new TransportError(error.message)));
    this.process.stdout.on('error', error => this.fail(new TransportError(error.message)));
    this.process.stderr.on('error', error => this.fail(new TransportError(error.message)));
    this.process.stderr.on('data', chunk => {
      this.diagnostics = Buffer.concat([this.diagnostics, chunk]).subarray(-65536);
    });
    this.process.stdout.on('data', chunk => this.read(chunk));
    this.process.stdout.on('end', () => this.fail(this.length ?
      new ProtocolError('unterminated response') : new TransportError('server stdout closed')));
  }

  get stderr() { return this.diagnostics.toString('utf8'); }

  read(chunk) {
    if (this.failure) return;
    try {
      let start = 0;
      while (start < chunk.length) {
        const end = chunk.indexOf(10, start);
        const part = chunk.subarray(start, end < 0 ? chunk.length : end);
        const required = this.length + part.length;
        if (required > FRAME_BYTES) throw new ProtocolError('response exceeds 8 MiB');
        if (required > this.buffer.length) {
          const grown = Buffer.allocUnsafe(Math.min(FRAME_BYTES, Math.max(required, this.buffer.length * 2)));
          this.buffer.copy(grown, 0, 0, this.length);
          this.buffer = grown;
        }
        part.copy(this.buffer, this.length);
        this.length = required;
        if (end < 0) break;
        const message = decode(new TextDecoder('utf-8', { fatal: true }).decode(this.buffer.subarray(0, this.length)));
        this.length = 0;
        this.reply(message);
        start = end + 1;
      }
    } catch (error) {
      this.fail(error instanceof ProtocolError ? error : new ProtocolError(error.message));
    }
  }

  reply(message) {
    if (!object(message) || message.jsonrpc !== '2.0') throw new ProtocolError('invalid JSON-RPC envelope');
    if (!Object.hasOwn(message, 'id') && typeof message.method === 'string') return;
    const entry = this.pending.get(message.id);
    if (!integer(message.id) || !entry || Object.hasOwn(message, 'method')) throw new ProtocolError('unexpected response identity');
    if (Object.hasOwn(message, 'result') === Object.hasOwn(message, 'error')) {
      throw new ProtocolError('response must contain exactly one result or error');
    }
    if (Object.hasOwn(message, 'error') && (!object(message.error) ||
        !Number.isSafeInteger(message.error.code) || typeof message.error.message !== 'string')) {
      throw new ProtocolError('invalid RPC error');
    }
    this.pending.delete(message.id);
    this.bytes -= entry.size;
    entry.finish(message.error ? new RpcError(message.error.code, message.error.message, message.error.data) : null, message.result);
  }

  request(method, params, { timeout = this.timeout, signal } = {}) {
    positive(timeout, 'timeout');
    signal?.throwIfAborted();
    if (this.failure) return Promise.reject(this.failure);
    const id = this.next + 1;
    if (!integer(id)) return Promise.reject(new TransportError('request IDs exhausted'));
    const wire = Buffer.from(encode({ jsonrpc: '2.0', id, method, params }) + '\n');
    if (wire.length > FRAME_BYTES || this.pending.size >= 64 || this.bytes + wire.length > FRAME_BYTES) {
      return Promise.reject(new TransportError('request or pending-request budget exceeded'));
    }
    this.next = id;
    this.bytes += wire.length;
    return new Promise((resolve, reject) => {
      let settled = false;
      const finish = (error, value, failed = error !== null) => {
        if (settled) return;
        settled = true;
        clearTimeout(timer);
        signal?.removeEventListener('abort', abort);
        failed ? reject(error) : resolve(value);
      };
      const abort = () => finish(signal.reason, undefined, true);
      const timer = setTimeout(() => finish(new RequestTimeout('local request deadline exceeded; remote outcome unknown')), timeout);
      signal?.addEventListener('abort', abort, { once: true });
      // Keep timed-out entries until their late replies arrive, under the same quota.
      this.pending.set(id, { size: wire.length, finish });
      this.process.stdin.write(wire, error => {
        if (error) this.fail(new TransportError(error.message));
      });
    });
  }

  initialized() {
    if (this.failure) throw this.failure;
    this.process.stdin.write('{"jsonrpc":"2.0","method":"notifications/initialized"}\n');
  }

  fail(error) {
    if (!this.failure) this.failure = error;
    for (const entry of this.pending.values()) entry.finish(this.failure);
    this.pending.clear();
    this.bytes = 0;
    this.buffer = Buffer.allocUnsafe(8192);
    this.length = 0;
    if (!this.closing) this.closing = this.shutdown();
  }

  kill(signal) {
    try {
      if (this.process.pid && process.platform !== 'win32') process.kill(-this.process.pid, signal);
      else this.process.kill(signal);
    } catch (error) {
      if (error.code !== 'ESRCH') throw error;
    }
  }

  async shutdown() {
    this.process.stdin.end();
    if (!await this.waitExit(2000)) {
      this.kill('SIGTERM');
      if (!await this.waitExit(1000)) this.kill('SIGKILL');
    }
    // Kill remaining group members even when the leader already exited.
    this.kill('SIGKILL');
    this.process.stdin.destroy();
    this.process.stdout.destroy();
    this.process.stderr.destroy();
    await this.exited;
  }

  async waitExit(ms) {
    const timer = new AbortController();
    try {
      return await Promise.race([this.exited.then(() => true), delay(ms, false, { signal: timer.signal })]);
    } finally { timer.abort(); }
  }

  async close() {
    this.fail(new TransportError('client closed'));
    await this.closing;
  }
}
