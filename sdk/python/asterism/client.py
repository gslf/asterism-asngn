"""Async host API. Task handles belong to this live server, not to the store."""
import asyncio
from collections.abc import AsyncIterator, Sequence
from .contract import STATES, integer, payload, poll, task_record
from .errors import ProtocolError, RequestTimeout
from .transport import Transport, positive
from .types import Approval, Definition, Poll, WorkState, TaskRecord


class Client:
    def __init__(self, transport: Transport):
        self._rpc = transport
        self.server_info: dict = {}

    @classmethod
    async def start(cls, command: Sequence[str], *, timeout: float = 10,
                    cwd: str | None = None, env: dict[str, str] | None = None) -> "Client":
        client = cls(await Transport.start(command, timeout=timeout, cwd=cwd, env=env))
        try:
            reply = await client._rpc.request("initialize", {
                "protocolVersion": "2025-06-18", "capabilities": {},
                "clientInfo": {"name": "asterism-python", "version": "0.1.0"}})
            try:
                version = reply["capabilities"]["experimental"]["dev.asterism/asngn"]["contractVersion"]
                valid = (reply["protocolVersion"] == "2025-06-18" and
                         reply["serverInfo"]["name"] == "asngn-mcp" and
                         isinstance(reply["serverInfo"]["version"], str) and type(version) is int and version == 1)
            except (KeyError, TypeError):
                valid = False
            if not valid:
                raise ProtocolError("unsupported Asterism contract; version 1 required")
            client.server_info = reply["serverInfo"]
            await client._rpc.initialized()
            return client
        except BaseException:
            await client.close()
            raise

    @property
    def stderr(self) -> str:
        """Last 64 KiB of diagnostics, never automatically copied into exceptions."""
        return self._rpc.stderr

    async def close(self) -> None:
        await self._rpc.close()

    async def __aenter__(self) -> "Client":
        return self

    async def __aexit__(self, *_args) -> None:
        await self.close()

    async def call_tool(self, name: str, arguments: dict, *, timeout: float | None = None) -> dict:
        return payload(await self._rpc.request("tools/call", {"name": name, "arguments": arguments}, timeout=timeout))

    def session(self, slug: str = "main") -> "Session":
        """Select a session; the server opens it on the first operation."""
        if not isinstance(slug, str) or not slug or "\0" in slug:
            raise ValueError("session slug must be a nonempty string without NUL")
        return Session(self, slug)

    def task(self, task_id: str) -> "Task":
        """Attach another observer to a handle on this same live server."""
        if not isinstance(task_id, str) or not task_id or "\0" in task_id:
            raise ValueError("task_id must be a nonempty string without NUL")
        return Task(self, task_id)


class Session:
    def __init__(self, client: Client, slug: str):
        self.client, self.slug = client, slug

    async def submit(self, message: str, *, timeout: float | None = None) -> "Task":
        result = await self.client.call_tool("agent_submit", {"session": self.slug, "message": message}, timeout=timeout)
        if not isinstance(result.get("task_id"), str) or not result["task_id"]:
            raise ProtocolError("submit returned no task handle")
        return self.client.task(result["task_id"])

    async def recover(self, task_id: str, *, timeout: float | None = None) -> TaskRecord:
        """Read durable evidence from an idle session; never rerun the task."""
        self.client.task(task_id)  # Reuse the public identity validation.
        value = await self.client.call_tool("agent_recover", {"session": self.slug, "task_id": task_id}, timeout=timeout)
        return task_record(value, task_id)

    async def work(self) -> WorkState:
        return await self._work({"mode": "get"})

    async def define_work(self, expected_revision: int, definition: Definition) -> WorkState:
        return await self._work({"mode": "define", "expected_revision": expected_revision, "definition": definition})

    async def invalidate_work(self, expected_revision: int) -> WorkState:
        return await self._work({"mode": "invalidate", "expected_revision": expected_revision})

    async def _work(self, arguments) -> WorkState:
        value = await self.client.call_tool("session_work", {"session": self.slug, **arguments})
        if not isinstance(value.get("task_state"), str) or value["task_state"] not in STATES:
            raise ProtocolError("invalid acceptance state")
        return value

    async def approval(self) -> Approval:
        value = await self.client.call_tool("session_approval", {"session": self.slug})
        if not isinstance(value.get("status"), str) or value["status"] not in {"none", "pending", "approved", "denied", "consumed", "invalidated", "interrupted"}:
            raise ProtocolError("invalid approval state")
        return value


class Task:
    def __init__(self, client: Client, task_id: str):
        self.client, self.id = client, task_id

    async def poll(self, cursor: int = 0, *, timeout: float | None = None) -> Poll:
        if not integer(cursor):
            raise ValueError("cursor must be a nonnegative safe integer")
        result = await self.client.call_tool("agent_poll", {"task_id": self.id, "cursor": cursor}, timeout=timeout)
        return poll(result, self.id, cursor)

    async def cancel(self) -> Poll:
        """Request remote cancellation; completion may require subsequent polling."""
        return poll(await self.client.call_tool("agent_cancel", {"task_id": self.id}), self.id, 0)

    async def release(self) -> Poll:
        """Free a completed handle. A running handle stays available."""
        return poll(await self.client.call_tool("agent_release", {"task_id": self.id}), self.id, 0)

    async def updates(self, cursor: int = 0, *, interval: float = 0.1,
                      timeout: float | None = None) -> AsyncIterator[Poll]:
        positive(interval, "interval")
        loop = asyncio.get_running_loop()
        deadline = None if timeout is None else loop.time() + positive(timeout, "timeout")
        while True:
            remaining = None if deadline is None else deadline - loop.time()
            if remaining is not None and remaining <= 0:
                raise RequestTimeout("local task wait expired; task remains available")
            page = await self.poll(cursor, timeout=min(self.client._rpc.timeout, remaining) if remaining is not None else None)
            yield page
            if page["done"]:
                return
            cursor = page["next_cursor"]
            await asyncio.sleep(interval if deadline is None else max(0, min(interval, deadline - loop.time())))

    async def wait(self, *, interval: float = 0.1, timeout: float | None = None) -> Poll:
        """Return the terminal packet, including non-success engine outcomes."""
        async for page in self.updates(interval=interval, timeout=timeout):
            if page["done"]:
                return page
        raise ProtocolError("task stream ended without an outcome")
