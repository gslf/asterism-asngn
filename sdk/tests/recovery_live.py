"""Real stdio SDK recovery after a missing-model failure; no inference or socket."""
import asyncio
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import uuid

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "python"))
from asterism import Client, RpcError, ToolError


async def submit(command):
    async with await Client.start(command) as client:
        session = client.session("archive")
        task = await session.submit("hi")
        result = await task.wait(timeout=10)
        assert result["outcome"] != "ASNGN_OK", result
        await task.release()
        record = await session.recover(task.id)
        assert record["state"] == "finished" and record["outcome"] == result["outcome"], record
        assert record["answer"] == result["answer"] and not record["turn_committed"], record
        try:
            await client.call_tool("agent_recover", {"session": "archive", "task_id": task.id, "resume": True})
            raise AssertionError("unexpected replay field was accepted")
        except RpcError as error:
            assert error.code == -32602
        return record


async def recover(command, record, interrupted):
    async with await Client.start(command) as client:
        session = client.session("archive")
        assert await session.recover(record["task_id"]) == record
        pending = await session.recover(interrupted)
        assert pending["state"] == "interrupted" and pending["action_uncertain"], pending
        assert pending["last_action"] == "edit.apply {patch: pending α}" and "outcome" not in pending
        assert not pending["execution_resumed"] and not pending["events_replayed"]
        for call in (client.task(record["task_id"]).poll(), session.recover(str(uuid.uuid4()))):
            try:
                await call
                raise AssertionError("unknown handle was attached")
            except ToolError as error:
                assert error.code == "ASNGN_ERR_NOT_FOUND"


def frame(value):
    """Construct a checksummed interrupted-action fixture, never an acceptance proof."""
    body = json.dumps(value, ensure_ascii=False).encode()
    prefix = f"// asngn-wal-v2 {len(body)} {hashlib.sha256(body).hexdigest()}".encode()
    return prefix + b" " + hashlib.sha256(prefix).hexdigest().encode() + b"\n" + body + b"\n"


with tempfile.TemporaryDirectory(prefix="asngn-recovery-sdk-") as temporary:
    root = Path(temporary)
    config = {"integration": {"asper": {"enable": False}, "astools": {"enable": False}},
              "cache": {"enable": False}, "validation": {"judge": "off"},
              "routing": {"classifier": "heuristic"}, "models": {"pool": [
                  {"id": name, "path": str(root / "absent.gguf")} for name in ("nano", "light", "std")]}}
    path = root / "config.xcdn"
    path.write_text("#asngn_config " + json.dumps(config))
    command = [sys.argv[1], "--root", str(root / "engine"), "--config", str(path)]
    record = asyncio.run(submit(command))
    journal = root / "engine/sessions/archive/turns.xcdn"
    interrupted = str(uuid.uuid4())
    with journal.open("ab") as output:
        output.write(frame({"id": interrupted, "state": "started", "schema": 1,
                            "work_revision": 0, "input": "Repair α"}))
        output.write(frame({"id": interrupted, "state": "action", "schema": 1,
                            "action_id": str(uuid.uuid4()), "input": "edit.apply {patch: pending α}"}))
    before = journal.read_bytes()
    asyncio.run(recover(command, record, interrupted))
    if len(sys.argv) > 2:
        subprocess.run([sys.argv[2], str(Path(__file__).with_suffix(".mjs")),
                        json.dumps(command), json.dumps(record), interrupted], check=True, timeout=30)
    assert journal.read_bytes() == before, "recovery appended or replayed journal records"
print("SDK recovery: durable failure, interrupted action, restart and unchanged journal passed")
