"""Bounded, multiplexed JSON-RPC over a child process. No shell is involved."""
import asyncio
import json
import math
import os
import signal
from .errors import ProtocolError, RequestTimeout, RpcError, TransportError

FRAME_BYTES = 8 * 1024 * 1024
PENDING_MAX = 64


def positive(value, name):
    if isinstance(value, bool) or not isinstance(value, (int, float)) or not math.isfinite(value) or value <= 0:
        raise ValueError(f"{name} must be finite and positive")
    return value


def _object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError("duplicate JSON key")
        result[key] = value
    return result


def decode(text):
    def invalid(_value):
        raise ValueError("non-finite JSON number")
    def number(value):
        parsed = float(value)
        if not math.isfinite(parsed):
            raise ValueError("non-finite JSON number")
        return parsed
    return json.loads(text, object_pairs_hook=_object, parse_constant=invalid, parse_float=number)


class Transport:
    def __init__(self, process, timeout):
        self.process, self.timeout = process, timeout
        self._next, self._bytes, self._pending = 0, 0, {}
        self._failure, self._closing = None, None
        self._stderr = bytearray()
        self._readers = [asyncio.create_task(self._read()), asyncio.create_task(self._drain_stderr())]

    @classmethod
    async def start(cls, command, *, timeout=10, cwd=None, env=None):
        positive(timeout, "timeout")
        if isinstance(command, (str, bytes)) or not command:
            raise ValueError("command must be a nonempty argv sequence")
        process = await asyncio.create_subprocess_exec(
            *command, cwd=cwd, env=env, start_new_session=os.name == "posix",
            stdin=asyncio.subprocess.PIPE, stdout=asyncio.subprocess.PIPE,
            stderr=asyncio.subprocess.PIPE, limit=FRAME_BYTES + 1)
        return cls(process, timeout)

    @property
    def stderr(self):
        return self._stderr.decode("utf-8", errors="replace")

    async def _drain_stderr(self):
        while chunk := await self.process.stderr.read(8192):
            self._stderr.extend(chunk)
            del self._stderr[:-65536]

    def _fail(self, error):
        if self._failure is None:
            self._failure = error
        for future, _size in self._pending.values():
            if not future.done():
                future.set_exception(self._failure)
        self._pending.clear()
        self._bytes = 0
        if self._closing is None:
            self._closing = asyncio.create_task(self._shutdown())

    async def _read(self):
        try:
            while line := await self.process.stdout.readline():
                if len(line) > FRAME_BYTES + 1 or not line.endswith(b"\n"):
                    raise ProtocolError("oversized or unterminated response")
                message = decode(line.decode("utf-8"))
                if not isinstance(message, dict) or message.get("jsonrpc") != "2.0":
                    raise ProtocolError("invalid JSON-RPC envelope")
                # ⁂ asterism currently sends no notifications. Ignore valid future ones.
                if "id" not in message and isinstance(message.get("method"), str):
                    continue
                identity = message.get("id")
                if type(identity) is not int or identity not in self._pending or "method" in message:
                    raise ProtocolError("unexpected response identity")
                if ("result" in message) == ("error" in message):
                    raise ProtocolError("response must contain exactly one result or error")
                error = message.get("error")
                if "error" in message and (not isinstance(error, dict) or
                        type(error.get("code")) is not int or not isinstance(error.get("message"), str)):
                    raise ProtocolError("invalid RPC error")
                future, size = self._pending.pop(identity)
                self._bytes -= size
                if not future.done():
                    if error is not None:
                        future.set_exception(RpcError(error["code"], error["message"], error.get("data")))
                    else:
                        future.set_result(message["result"])
            self._fail(TransportError("server stdout closed"))
        except (ValueError, UnicodeError, RecursionError) as error:
            self._fail(ProtocolError(str(error)))
        except (OSError, ProtocolError) as error:
            self._fail(error if isinstance(error, ProtocolError) else TransportError(str(error)))

    async def request(self, method, params, *, timeout=None):
        duration = positive(self.timeout if timeout is None else timeout, "timeout")
        if self._failure:
            raise self._failure
        identity = self._next + 1
        wire = (json.dumps(dict(jsonrpc="2.0", id=identity, method=method, params=params),
                           ensure_ascii=False, allow_nan=False, separators=(",", ":")) + "\n").encode("utf-8")
        if len(wire) > FRAME_BYTES or len(self._pending) >= PENDING_MAX or self._bytes + len(wire) > FRAME_BYTES:
            raise TransportError("request or pending-request budget exceeded")
        future = asyncio.get_running_loop().create_future()
        self._pending[identity] = (future, len(wire))
        self._next, self._bytes = identity, self._bytes + len(wire)
        try:
            async with asyncio.timeout(duration):
                # write() does not yield: concurrent callers cannot interleave frames.
                self.process.stdin.write(wire)
                await self.process.stdin.drain()
                return await asyncio.shield(future)
        except TimeoutError as error:
            raise RequestTimeout("local request deadline exceeded; remote outcome unknown") from error
        except (BrokenPipeError, ConnectionError) as error:
            self._fail(TransportError(str(error)))
            raise self._failure from error
        finally:
            # Keep a bounded tombstone until a late reply arrives. Never reuse IDs.
            if not future.done():
                future.cancel()
            elif not future.cancelled():
                future.exception()  # Consume failures arriving while drain was interrupted.

    async def initialized(self):
        if self._failure:
            raise self._failure
        self.process.stdin.write(b'{"jsonrpc":"2.0","method":"notifications/initialized"}\n')
        await self.process.stdin.drain()

    def _signal(self, *, kill=False):
        try:
            if os.name == "posix":
                os.killpg(self.process.pid, signal.SIGKILL if kill else signal.SIGTERM)
            elif self.process.returncode is None:
                self.process.kill() if kill else self.process.terminate()
        except ProcessLookupError:
            pass

    async def _shutdown(self):
        self.process.stdin.close()
        try:
            await asyncio.wait_for(self.process.wait(), 2)
        except TimeoutError:
            self._signal()
            try:
                await asyncio.wait_for(self.process.wait(), 1)
            except TimeoutError:
                self._signal(kill=True)
        finally:
            # Descendants may retain pipes after the leader exits.
            self._signal(kill=True)
            for task in self._readers:
                task.cancel()
            await asyncio.gather(*self._readers, return_exceptions=True)
            await asyncio.wait_for(self.process.wait(), 2)

    async def close(self):
        self._fail(TransportError("client closed"))
        await asyncio.shield(self._closing)
