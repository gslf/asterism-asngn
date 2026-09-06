"""ACP admission tests against the actual executable and the same host with fake models."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile
from peer import Peer


def production(binary):
    with tempfile.TemporaryDirectory(prefix="asngn-acp-production-") as directory:
        workspace = Path(directory) / "workspace"
        workspace.mkdir()
        with Peer([binary, "--root", str(Path(directory) / "state"),
                   "--workspace", str(workspace), "--allow-degraded"], workspace=workspace) as peer:
            peer.request(0, "session/new", cwd=str(workspace), mcpServers=[])
            assert peer.until(0)["error"]["code"] == -32000
            peer.initialize()
            session = peer.session()
            peer.prompt(session)
            result = peer.until(3)
            assert "error" in result and result["error"]["data"]["outcome"] != "ASNGN_OK", result
            peer.request(4, "session/close", sessionId=session)
            assert peer.until(4)["result"] == {}
            peer.close()
    assert subprocess.run([binary], capture_output=True, timeout=5).returncode == 2


def admission(driver):
    with Peer([driver, "answer"], fixture=True) as peer:
        # Byte fragmentation includes a UTF-8 sequence split between input reads.
        wire = (json.dumps(dict(jsonrpc="2.0", id=1, method="initialize", params={
            "protocolVersion": 1, "clientInfo": {"name": "è 🍵", "version": "1"}}),
            ensure_ascii=False) + "\n").encode()
        for byte in wire:
            peer.raw(bytes([byte]))
        assert peer.until(1)["result"]["protocolVersion"] == 1
        invalid_sessions = [dict(cwd="relative", mcpServers=[]),
            dict(cwd="/", mcpServers=[]),
            dict(cwd=str(peer.workspace), mcpServers=[{"command": "touch", "args": ["owned"]}]),
            dict(cwd=str(peer.workspace), mcpServers=[], additionalDirectories=["/tmp"]),
            dict(cwd=str(peer.workspace), mcpServers=None)]
        for ident, params in enumerate(invalid_sessions, 20):
            peer.request(ident, "session/new", **params)
            assert peer.until(ident)["error"]["code"] == -32602
        session = peer.session()
        for ident, blocks in enumerate(([], [{"type": "text", "text": "a\0b"}],
                [{"type": "image", "data": "abc", "mimeType": "image/png"}],
                [{"type": "resource_link", "uri": "file:///tmp/a"}],
                [{"type": "text", "text": "x" * 262144}]), 30):
            peer.request(ident, "session/prompt", sessionId=session, prompt=blocks)
            assert "error" in peer.until(ident)
        peer.send(dict(jsonrpc="2.0", method="session/prompt", params={
            "sessionId": session, "prompt": [{"type": "text", "text": "ignored notification"}]}))
        peer.request(40, "session/prompt", sessionId=session, prompt=[
            {"type": "text", "text": "hello"},
            {"type": "resource_link", "name": "example", "uri": "file:///outside/read-not-authorized"}])
        result = peer.until(40)["result"]
        assert result["stopReason"] == "end_turn"
        assert result["_meta"]["dev.asterism/asngn"]["task_state"] == "unconfirmed"
        output = "".join(v["content"]["text"] for v in peer.updates()
                         if v["sessionUpdate"] == "agent_message_chunk")
        assert output == "Hello α 🍵.\n", repr(output)
        peer.request(41, "session/close", sessionId=session)
        assert peer.until(41)["result"] == {}
        peer.prompt(session, 42)
        assert peer.until(42)["error"]["data"]["outcome"] == "ASNGN_ERR_NOT_FOUND"
        assert "tool calls: 0" in peer.close()


def framing(driver):
    for payload, expected in ((b'{"jsonrpc":', 1), (b"x" * (1024 * 1024 + 1), 1)):
        with Peer([driver, "answer"], fixture=True) as peer:
            peer.raw(payload)
            peer.close(expected)
    with Peer([driver, "answer"], fixture=True) as peer:
        peer.raw(b'{"jsonrpc":"2.0","id":1,"id":2,"method":"initialize"}\n')
        assert peer.receive()["error"]["code"] == -32700
        peer.raw(b'[]\n')
        assert peer.receive()["error"]["code"] == -32600
        peer.initialize()
        for ident in (None, "è 🍵", 9007199254740992, 9007199254740993, -(2**63), 2**63-1):
            peer.request(ident, "unknown", value="ignored")
            assert peer.until(ident)["error"]["code"] == -32601
        for ident in (True, 1.5, 2**63, "a\0b", "x"*129):
            peer.request(ident, "session/new", cwd=str(peer.workspace), mcpServers=[])
            assert peer.receive()["error"]["code"] == -32600
        sessions = [peer.session(100+i) for i in range(8)]
        peer.request(200, "session/new", cwd=str(peer.workspace), mcpServers=[])
        assert peer.until(200)["error"]["data"]["outcome"] == "ASNGN_ERR_LIMIT"
        peer.request(201, "session/close", sessionId=sessions[0])
        assert peer.until(201)["result"] == {}
        assert peer.session(202) not in sessions
        peer.close()


if __name__ == "__main__":
    production(sys.argv[1])
    admission(sys.argv[2])
    framing(sys.argv[2])
    print("ACP production startup, framing, capabilities, admission and session limits passed")
