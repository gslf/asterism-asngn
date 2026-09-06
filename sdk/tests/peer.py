"""Adversarial child used only to test SDK transport boundaries."""
import json
import os
import signal
import sys
import threading
import time

lock = threading.Lock()


def emit(value):
    with lock:
        sys.stdout.write(json.dumps(value, ensure_ascii=False) + "\n")
        sys.stdout.flush()


def result(identity, value, error=False):
    emit({"jsonrpc": "2.0", "id": identity, "result": {
        "content": [{"type": "text", "text": json.dumps(value)}], "isError": error}})


for line in sys.stdin:
    request = json.loads(line)
    if "id" not in request:
        continue
    identity = request["id"]
    if request["method"] == "initialize":
        emit({"jsonrpc": "2.0", "id": identity, "result": {
            "protocolVersion": "2025-06-18", "serverInfo": {"name": "asngn-mcp", "version": "fixture"},
            "capabilities": {"experimental": {"dev.asterism/asngn": {"contractVersion": 1}}}}})
        continue
    args = request["params"]["arguments"]
    name = request["params"]["name"]
    if name == "hold":
        continue
    if name == "late":
        def late(i=identity):
            time.sleep(0.2)
            result(i, {"late": True})
        threading.Thread(target=late, daemon=True).start()
        continue
    if name == "stderr":
        sys.stderr.write("x" * 150000 + "α-end")
        sys.stderr.flush()
    if name == "orphan":
        child = os.fork()
        if child == 0:
            signal.signal(signal.SIGTERM, signal.SIG_IGN)
            while True:
                time.sleep(1)
        result(identity, {"pid": child})
        continue
    if name == "rpc_error":
        emit({"jsonrpc": "2.0", "id": identity, "error": {"code": -32602, "message": "bad argument", "data": {"field": "x"}}})
        continue
    if name == "denied":
        result(identity, {"error": "ASNGN_ERR_DENIED", "message": "denied"}, True)
        continue
    if name in ("malformed", "duplicate", "badutf8", "oversize", "wrong_id", "unterminated", "exit"):
        raw = {"malformed": b'[]\n', "duplicate": b'{"jsonrpc":"2.0","id":2,"id":2,"result":{}}\n',
               "badutf8": b'\xff\n', "oversize": b'x' * (8 * 1024 * 1024 + 2),
               "wrong_id": b'{"jsonrpc":"2.0","id":9999,"result":{}}\n',
               "unterminated": b'{"jsonrpc":"2.0"}', "exit": b''}[name]
        sys.stdout.buffer.write(raw)
        sys.stdout.buffer.flush()
        if name in ("unterminated", "exit"):
            break
        continue
    result(identity, args)
