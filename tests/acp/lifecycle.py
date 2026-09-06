"""Actual tool execution must follow the editor's exact, current permission decision."""
import json
import sys
from peer import Peer


def pending(peer, session):
    peer.prompt(session, text="Run fake.mut")
    for _ in range(128):
        packet = peer.receive()
        peer.events.append(packet)
        if packet.get("method") == "session/request_permission":
            params = packet["params"]
            assert params["sessionId"] == session
            assert [v["kind"] for v in params["options"]] == ["allow_once", "reject_once"]
            call = params["toolCall"]
            assert call["toolCallId"] == packet["id"] and call["status"] == "pending"
            assert "review α payload" in call["rawInput"]["arguments"]
            assert len(call["rawInput"]["arguments_sha256"]) == 64
            assert len(call["rawInput"]["snapshot"]) == 64
            assert call["rawInput"]["workspace"] == str(peer.workspace)
            return packet["id"]
    raise AssertionError("No permission request")


def decision(driver, mode):
    with Peer([driver, "mutate"], fixture=True) as peer:
        peer.initialize()
        session = peer.session()
        permission = pending(peer, session)
        # A forged/early permission ID cannot authorize the real pending action.
        peer.send(dict(jsonrpc="2.0", id="00000000-0000-4000-8000-000000000000",
            result={"outcome": {"outcome": "selected", "optionId": "allow-once"}}))
        if mode == "stale":
            (peer.workspace / "external.txt").write_text("new snapshot", encoding="utf-8")
        if mode in ("cancel", "close"):
            if mode == "cancel":
                peer.send(dict(jsonrpc="2.0", method="session/cancel", params={"sessionId": session}))
            else:
                peer.request(4, "session/close", sessionId=session)
        else:
            option = "reject-once" if mode == "reject" else "allow-always" if mode == "invalid" else "allow-once"
            reply = dict(jsonrpc="2.0", id=permission,
                         result={"outcome": {"outcome": "selected", "optionId": option}})
            if mode == "permission-cancel":
                reply["result"] = {"outcome": {"outcome": "cancelled"}}
            peer.send(reply)
            # An identical replay must never dispatch the action twice.
            peer.send(reply)
        terminal = peer.until(3)
        assert terminal["result"]["stopReason"] == (
            "cancelled" if mode in ("cancel", "close", "invalid", "permission-cancel") else "end_turn"), terminal
        calls = [v for v in peer.updates() if v.get("toolCallId") == permission]
        states = [v["status"] for v in calls]
        if mode == "allow":
            assert states == ["pending", "in_progress", "completed"], states
            action_ids = [v["_meta"]["dev.asterism/asngn"]["action_id"] for v in calls[1:]]
            assert action_ids[0] == action_ids[1] and action_ids[0] != permission
            assert all(v["_meta"]["dev.asterism/asngn"]["journaled"] for v in calls[1:])
        else:
            assert states == ["pending", "failed"], states
        assert terminal["result"]["_meta"]["dev.asterism/asngn"]["task_state"] == "unconfirmed"
        if mode == "close":
            assert peer.until(4)["result"] == {}
            peer.prompt(session, 5)
            assert peer.until(5)["error"]["data"]["outcome"] == "ASNGN_ERR_NOT_FOUND"
        assert f"tool calls: {1 if mode == 'allow' else 0}" in peer.close()


def read_and_reuse(driver):
    with Peer([driver, "read"], fixture=True) as peer:
        peer.initialize()
        session = peer.session()
        for ident in (3, 4):
            peer.prompt(session, ident, text=f"Run fake.run, observation {ident}")
            assert peer.until(ident)["result"]["stopReason"] == "end_turn"
        assert all(v.get("method") != "session/request_permission" for v in peer.events)
        calls = [v for v in peer.updates() if v["sessionUpdate"].startswith("tool_call")]
        assert [v["status"] for v in calls] == ["in_progress", "completed"] * 2, calls
        assert calls[0]["toolCallId"] != calls[2]["toolCallId"]
        assert "tool calls: 2" in peer.close()


def cancellation_is_responsive(driver):
    with Peer([driver, "mutate"], fixture=True) as peer:
        peer.initialize()
        session = peer.session()
        pending(peer, session)
        peer.prompt(session, 4)
        assert peer.until(4)["error"]["data"]["outcome"] == "ASNGN_ERR_BUSY"
        # A partial frame is deliberately left pending while the turn is cancelled.
        peer.raw(b'{"jsonrpc":"2.0","method":"session/cancel","params":{"sessionId":')
        peer.raw(json.dumps(session).encode() + b'}}\n')
        assert peer.until(3)["result"]["stopReason"] == "cancelled"
        peer.request(5, "session/close", sessionId=session)
        assert peer.until(5)["result"] == {}
        peer.session(6)
        assert "tool calls: 0" in peer.close()
    with Peer([driver, "mutate"], fixture=True) as peer:
        peer.initialize()
        pending(peer, peer.session())
        assert "tool calls: 0" in peer.close()  # Disconnect while awaiting approval.


def output(driver):
    with Peer([driver, "long"], fixture=True) as peer:
        peer.initialize()
        session = peer.session()
        peer.prompt(session)
        peer.raw(b'{"jsonrpc":')  # Incomplete input must not block answer delivery.
        assert peer.until(3)["result"]["stopReason"] == "end_turn"
        chunks = [v for v in peer.updates() if v["sessionUpdate"] == "agent_message_chunk"]
        answer = "".join(v["content"]["text"] for v in chunks)
        assert answer == "α 🍵 evidence.\n" * 6000, (len(answer), answer[-100:])
        assert len({v["messageId"] for v in chunks}) == 1
        peer.raw(b'"2.0","id":4,"method":"unsupported","params":{}}\n')
        assert peer.until(4)["error"]["code"] == -32601
        peer.close()


def concurrent_sessions(driver):
    with Peer([driver, "answer"], fixture=True) as peer:
        peer.initialize()
        sessions = [peer.session(10+i) for i in range(2)]
        for index, session in enumerate(sessions):
            peer.prompt(session, 20+index, text=f"hello {index}")
        terminals = [peer.until(20+i)["result"] for i in range(2)]
        ids = [r["_meta"]["dev.asterism/asngn"]["task_id"] for r in terminals]
        assert ids[0] != ids[1]
        for index, session in enumerate(sessions):
            chunks = [p["params"]["update"] for p in peer.events
                if p.get("method") == "session/update" and p["params"]["sessionId"] == session]
            assert "".join(p["content"]["text"] for p in chunks) == "Hello α 🍵.\n"
            assert all(p["messageId"] == ids[index] for p in chunks)
        peer.close()
    with Peer([driver, "error"], fixture=True) as peer:
        peer.initialize()
        peer.prompt(peer.session())
        terminal = peer.until(3)
        assert terminal["error"]["data"]["outcome"] == "ASNGN_ERR_MODEL", terminal
        peer.close()


if __name__ == "__main__":
    for scenario in ("allow", "reject", "stale", "cancel", "close", "invalid", "permission-cancel"):
        decision(sys.argv[1], scenario)
    read_and_reuse(sys.argv[1])
    cancellation_is_responsive(sys.argv[1])
    output(sys.argv[1])
    concurrent_sessions(sys.argv[1])
    print("ACP tool decisions, actual action IDs, stale snapshots, cancellation and stream recovery passed")
