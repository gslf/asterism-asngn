"""Bounded stdio editor peer. No network or production test hooks."""
import json
import os
from pathlib import Path
import selectors
import subprocess
import tempfile
import time


class Peer:
    def __init__(self, command, fixture=False, workspace=None):
        self.errors = tempfile.TemporaryFile()
        self.process = subprocess.Popen(command, stdin=subprocess.PIPE,
            stdout=subprocess.PIPE, stderr=self.errors, bufsize=0)
        self.selector = selectors.DefaultSelector()
        self.selector.register(self.process.stdout, selectors.EVENT_READ)
        self.buffer = b""
        self.events = []
        self.workspace = Path(self.receive()["fixtureWorkspace"]) if fixture else workspace

    def send(self, packet):
        self.raw((json.dumps(packet, ensure_ascii=False) + "\n").encode())

    def raw(self, data):
        view = memoryview(data)
        while view:
            written = self.process.stdin.write(view)
            if not written:
                raise AssertionError("Editor input stopped accepting bytes")
            view = view[written:]

    def request(self, ident, method, **params):
        self.send(dict(jsonrpc="2.0", id=ident, method=method, params=params))

    def receive(self, timeout=8):
        deadline = time.monotonic() + timeout
        while b"\n" not in self.buffer:
            assert len(self.buffer) <= 1024 * 1024, "Oversized server frame"
            remaining = deadline - time.monotonic()
            assert remaining > 0 and self.selector.select(remaining), "No ACP response"
            data = os.read(self.process.stdout.fileno(), 65536)
            if not data:
                self.errors.seek(0)
                raise AssertionError("ACP EOF: " + self.errors.read().decode(errors="replace"))
            self.buffer += data
        line, self.buffer = self.buffer.split(b"\n", 1)
        return json.loads(line)

    def until(self, ident):
        for index, packet in enumerate(self.events):
            if packet.get("id") == ident and "method" not in packet:
                return self.events.pop(index)
        for _ in range(4096):
            packet = self.receive()
            if packet.get("id") == ident and "method" not in packet:
                return packet
            self.events.append(packet)
        raise AssertionError("Unbounded responses before terminal result")

    def initialize(self):
        self.request(1, "initialize", protocolVersion=1,
                     clientInfo={"name": "test-editor", "version": "1"}, clientCapabilities={})
        result = self.until(1)["result"]
        assert result["protocolVersion"] == 1
        assert result["agentCapabilities"]["loadSession"] is False
        return result

    def session(self, ident=2):
        self.request(ident, "session/new", cwd=str(self.workspace), mcpServers=[])
        return self.until(ident)["result"]["sessionId"]

    def prompt(self, session, ident=3, text="hello"):
        self.request(ident, "session/prompt", sessionId=session,
                     prompt=[{"type": "text", "text": text}])

    def updates(self):
        return [v["params"]["update"] for v in self.events
                if v.get("method") == "session/update"]

    def close(self, expected=0):
        if not self.process.stdin.closed:
            self.process.stdin.close()
        status = self.process.wait(timeout=8)
        self.errors.seek(0)
        errors = self.errors.read().decode(errors="replace")
        assert status == expected, (status, errors)
        assert "runtime error:" not in errors and "ERROR: AddressSanitizer" not in errors, errors
        return errors

    def __enter__(self):
        return self

    def __exit__(self, *_args):
        if self.process.poll() is None:
            self.process.kill()
            self.process.wait(timeout=5)
        self.process.stdin.close()
        self.process.stdout.close()
        self.selector.close()
        self.errors.close()
