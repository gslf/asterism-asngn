"""Exercise host acceptance over the real stdio server, without inference."""
import json
import subprocess
import sys
import tempfile


def run(root, calls):
    messages = [dict(jsonrpc="2.0", id=1, method="initialize", params={
        "protocolVersion": "2025-06-18", "capabilities": {},
        "clientInfo": {"name": "acceptance-test", "version": "1"}})]
    messages += [dict(jsonrpc="2.0", id=i + 2, method="tools/call",
                     params={"name": "session_work", "arguments": args})
                 for i, args in enumerate(calls)]
    result = subprocess.run([sys.argv[1], "--root", root],
                            input="".join(json.dumps(m) + "\n" for m in messages),
                            text=True, capture_output=True, timeout=30, check=True)
    replies = [json.loads(line) for line in result.stdout.splitlines()]
    assert len(replies) == len(messages), result.stdout
    return [(r["result"]["isError"], json.loads(r["result"]["content"][0]["text"]))
            for r in replies[1:]]


with tempfile.TemporaryDirectory(prefix="asngn-work-smoke-") as root:
    define = {"mode": "define", "expected_revision": 0, "definition": {
        "goal": "Fix the regression", "constraints": "Keep the acceptance suite",
        "criteria": [{"id": "regression", "requirement": "Regression passes",
                      "command": "test", "adapter": "cmake", "path": ".", "depends_on": 0}]}}
    replies = run(root, [define, define, {"mode": "get"}])
    assert replies[0][0] is False and replies[0][1]["task_state"] == "incomplete"
    assert replies[1][0] is True and replies[1][1]["error"] == "ASNGN_ERR_BUSY"
    assert replies[2][1]["work_revision"] == 1
    replies = run(root, [{"mode": "get"}, {"mode": "invalidate", "expected_revision": 1}])
    assert replies[0][1]["work_revision"] == 1
    assert replies[0][1]["criteria"][0]["status"] == "not_run"
    assert replies[1][0] is False and replies[1][1]["work_revision"] == 2
