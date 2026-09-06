"""Both SDK transports inspect one recovered journal without inference."""
import asyncio
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import uuid

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "python"))
from asterism import Client, RpcError


def frame(value):
    data = json.dumps(value, separators=(",", ":")).encode()
    header = f"// asngn-wal-v2 {len(data)} {hashlib.sha256(data).hexdigest()}".encode()
    return header + b" " + hashlib.sha256(header).hexdigest().encode() + b"\n" + data + b"\n"


def records():
    for kind, reserve, known, outcome, input_tokens, output_tokens in (
            ("generate", 100, False, "ASNGN_ERR_CANCELLED", 7, 9),
            ("generate", 50, True, "ASNGN_OK", 10, 5),
            ("embed-query", 9007199254740993, False, None, 0, 0),
            ("embed-document", 30, True, "ASNGN_ERR_MODEL", 5, 2)):
        row = dict(schema=2, id=str(uuid.uuid4()), request_id="", model="fixture", kind=kind,
                   state="reserved", day=100, budget_delta=reserve, input_tokens=0,
                   output_tokens=0, usage_known=False, outcome="ASNGN_OK")
        yield frame(row)
        if outcome:
            yield frame({**row, "state": "settled", "budget_delta": input_tokens + output_tokens - reserve if known else 0,
                         "input_tokens": input_tokens, "output_tokens": output_tokens,
                         "usage_known": known, "outcome": outcome})


async def inspect(command):
    async with await Client.start(command) as client:
        usage = await client.consumption()
        assert usage["lifetime"]["charged_tokens"] == 9007199254741115
        assert usage["lifetime"]["unsettled_tokens"] == 9007199254740993
        expected = json.loads(Path(__file__).with_name("consumption.json").read_text())
        assert usage["lifetime"] == {k: int(v) for k, v in expected["lifetime"].items()}
        assert usage["today"] == {k: 0 for k in expected["today"]}
        for args in ({"session": "main"}, {"reset": True}):
            try:
                await client.call_tool("engine_consumption", args)
                raise AssertionError("invalid scope or mutation was admitted")
            except RpcError as error:
                assert error.code == -32602


with tempfile.TemporaryDirectory(prefix="asngn-consumption-sdk-") as td:
    root = Path(td)
    config = root / "config.xcdn"
    config.write_text('#asngn_config {integration:{asper:{enable:false},astools:{enable:false}}}')
    engine = root / "engine"
    engine.mkdir()
    journal = engine / "operations.xcdn"
    before = b"".join(records())
    journal.write_bytes(before)
    command = [sys.argv[1], "--root", str(engine), "--config", str(config)]
    asyncio.run(inspect(command))
    asyncio.run(inspect(command))  # Reopening does not double the counters.
    if len(sys.argv) > 2:
        subprocess.run([sys.argv[2], str(Path(__file__).with_suffix(".mjs")), json.dumps(command)],
                       check=True, timeout=30)
    assert journal.read_bytes() == before
    assert not (engine / "sessions/main").exists(), "inspection created a session"
print("Consumption: exact counters, both SDKs, repeated restart, unchanged journal passed")
