"""Approval inspection never grants the MCP tool client authority to approve."""
import json
import subprocess
import sys
import tempfile

with tempfile.TemporaryDirectory(prefix="asngn-approval-mcp-") as root:
    messages = [dict(jsonrpc="2.0", id=1, method="initialize", params={
        "protocolVersion": "2025-06-18", "capabilities": {},
        "clientInfo": {"name": "approval-test", "version": "1"}})]
    messages += [dict(jsonrpc="2.0", id=i+2, method="tools/call",
        params={"name": "session_approval", "arguments": args})
        for i, args in enumerate(({}, {"allow": True}, {}))]
    proc = subprocess.run([sys.argv[1], "--root", root],
        input="".join(json.dumps(v)+"\n" for v in messages), capture_output=True,
        text=True, encoding="utf-8", timeout=30, check=True)
    replies = [json.loads(line) for line in proc.stdout.splitlines()]
    assert replies[2]["error"]["code"] == -32602, replies
    for index in (1, 3):
        assert replies[index]["result"]["isError"] is False, replies
        value = json.loads(replies[index]["result"]["content"][0]["text"])
        assert value == {"status": "none"}, value
