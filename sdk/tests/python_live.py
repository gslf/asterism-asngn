"""SDK calls cross the actual MCP, scheduler, persistence and HTTP adapters."""
import asyncio
import json
import hashlib
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "python"))
from asterism import Client, RequestTimeout, RpcError, ToolError
from server_fixture import ANSWER, REVIEW, fixture

DEFINITION = {"goal": "Regression α", "constraints": "Keep original tests", "criteria": [{
    "id": "regression", "requirement": "The test must run", "command": "test", "adapter": "cmake", "path": ".", "depends_on": 0}]}


async def exercise(command, server):
    async with await Client.start(command) as client:
        session = client.session("sdk")
        assert (await session.approval())["status"] == "none"
        assert (await session.work())["task_state"] == "unconfirmed"
        state = await session.define_work(0, DEFINITION)
        assert state["work_revision"] == 1
        try:
            await session.define_work(0, DEFINITION)
            raise AssertionError("stale definition accepted")
        except ToolError as error:
            assert error.code == "ASNGN_ERR_BUSY"
        try:
            await client.call_tool("session_approval", {"session": "sdk", "allow": True})
            raise AssertionError("approval mutation accepted")
        except RpcError as error:
            assert error.code == -32602
        task = await session.submit("hello sdk-normal")
        pages = [page async for page in task.updates(timeout=10, interval=0.01)]
        result = pages[-1]
        assert result["done"] and result["outcome"] == "ASNGN_OK", result
        assert result["task_state"] == "incomplete", result
        assert result["answer"] == ANSWER, result
        assert any(event["truncated"] for page in pages for event in page["events"]), pages
        observer = client.task(task.id)
        assert (await observer.poll(result["next_cursor"]))["events"] == []
        await session.invalidate_work(1)
        assert (await task.poll())["task_state"] == "superseded"
        assert (await task.release())["done"]
        try:
            await observer.poll()
            raise AssertionError("released task remained available")
        except ToolError as error:
            assert error.code == "ASNGN_ERR_NOT_FOUND"
        slow = await client.session("slow").submit("hello sdk-slow")
        assert await asyncio.to_thread(server.started.wait, 5), "HTTP request did not start"
        try:
            await slow.wait(timeout=0.03, interval=0.01)
            raise AssertionError("local deadline ignored")
        except RequestTimeout:
            pass
        assert not (await slow.release())["done"], "release must not cancel a running task"
        await slow.cancel()
        cancelled = await slow.wait(timeout=10, interval=0.01)
        assert cancelled["outcome"] == "ASNGN_ERR_CANCELLED", cancelled
        await slow.release()
    async with await Client.start(command) as reopened:
        state = await reopened.session("sdk").work()
        assert state["work_revision"] == 2 and state["criteria"][0]["status"] == "not_run"
        assert (await reopened.session("sdk").approval())["status"] == "none"
        try:
            await reopened.task(task.id).poll()
            raise AssertionError("process-local handle survived restart")
        except ToolError as error:
            assert error.code == "ASNGN_ERR_NOT_FOUND"


async def approval(command):
    async with await Client.start(command) as client:
        session = client.session("review")
        task = await session.submit("Run wire.mut")
        for _ in range(500):
            value = await session.approval()
            if value["status"] == "pending":
                break
            await asyncio.sleep(0.01)
        assert value["status"] == "pending", value
        assert json.loads(value["arguments"])["msg"] == REVIEW and len(value["arguments"].encode()) > 4096
        assert hashlib.sha256(value["arguments"].encode()).hexdigest() == value["arguments_sha256"]
        assert len(value["arguments_sha256"]) == 64 and value["tool_ref"] == "wire@1.0.0"
        assert not (await task.poll())["done"]
        try:
            await client.call_tool("session_approval", {"session": "review", "allow": True})
            raise AssertionError("client granted its own approval")
        except RpcError as error:
            assert error.code == -32602
        assert (await session.approval())["status"] == "pending"
        await task.cancel()
        assert (await task.wait(timeout=10))["outcome"] == "ASNGN_ERR_CANCELLED"
        assert (await session.approval())["status"] == "interrupted"
        await task.release()
    async with await Client.start(command) as reopened:
        saved = await reopened.session("review").approval()
        assert saved["status"] == "interrupted" and saved["arguments"] == value["arguments"]


with fixture(sys.argv[1]) as (command, server):
    asyncio.run(exercise(command, server))
    assert len(server.requests) == 2, server.requests
print("Python SDK: live lifecycle, truncated events, cancellation and store reopen passed")

with fixture(sys.argv[1], sys.argv[2]) as (command, server):
    asyncio.run(approval(command))
    assert len(server.requests) == 2, server.requests
print("Python SDK: complete pending approval, denied escalation and durable interruption passed")
