"""Native proposals cross MCP, asngn, asmodel, HTTP and the actual tool dispatcher.

The HTTP peer is scripted: this measures contracts, never model quality.
"""
import json
import platform
import subprocess
import sys
import tempfile
import threading
import traceback
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *_args):
        pass

    def do_POST(self):
        try:
            request = json.loads(self.rfile.read(int(self.headers["content-length"])))
            self.server.requests.append(request)
            responses = self.path == "/v1/responses"
            assert self.path in ("/v1/responses", "/v1/chat/completions")
            tools = request.get("tools")
            calls = []
            text = ""
            if tools:
                assert request["tool_choice"] == "auto"
                definitions = tools if responses else [v["function"] for v in tools]
                messages = request["input" if responses else "messages"]
                results = [v for v in messages if v.get("type") == "function_call_output" or v.get("role") == "tool"]
                if results:
                    assert len(results) == 1
                    assert results[0].get("call_id", results[0].get("tool_call_id")) == "wire-read"
                    assert "wire observation" in results[0].get("output", results[0].get("content", ""))
                    name, call_id, args = "asterism_finish", "wire-finish", {}
                    if self.server.direct:
                        name = None
                        text = "Observed the tool result over the native protocol."
                else:
                    definition = next(v for v in definitions if v["description"].startswith("wire.run:"))
                    assert definition["parameters"]["required"] == ["msg"]
                    name, call_id, args = definition["name"], "wire-read", {"msg": "wire observation"}
                if name is not None:
                    calls = [{"name": name, "call_id": call_id, "arguments": json.dumps(args)}]
            elif len(self.server.requests) == 1:
                text = "CLASS MODERATE | DETAIL NORMAL | MODE PLAN | TASK LOOKUP\n" if "grammar" in request else json.dumps(
                    {"class": "MODERATE", "detail": "NORMAL", "mode": "PLAN", "task": "LOOKUP"})
            else:
                text = "Observed the tool result over the native protocol."
            if responses:
                output = [dict(type="function_call", **v) for v in calls] if calls else [
                    {"type": "message", "role": "assistant", "content": [{"type": "output_text", "text": text}]}]
                reply = {"status": "completed", "output": output, "usage": {
                    "input_tokens": 100, "output_tokens": 20, "output_tokens_details": {"reasoning_tokens": 0}}}
            else:
                message = {"role": "assistant", "content": text or None}
                if calls:
                    message["tool_calls"] = [{"id": v["call_id"], "type": "function",
                        "function": {"name": v["name"], "arguments": v["arguments"]}} for v in calls]
                reply = {"choices": [{"message": message, "finish_reason": "tool_calls" if calls else "stop"}],
                    "usage": {"prompt_tokens": 100, "completion_tokens": 20}}
            body = json.dumps(reply).encode()
            self.send_response(200)
            self.send_header("content-type", "application/json")
            self.send_header("content-length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
        except Exception:
            self.server.errors.append(traceback.format_exc())
            self.send_error(500)


def exercise(provider, direct):
    with tempfile.TemporaryDirectory(prefix="asngn-native-wire-") as tmp, ThreadingHTTPServer(("127.0.0.1", 0), Handler) as server:
        server.requests, server.errors, server.direct = [], [], direct
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        try:
            root = Path(tmp)
            workspace, registry = root / "workspace", root / "registry"
            workspace.mkdir()
            package = registry / "wire"
            package.mkdir(parents=True)
            os_name = {"Darwin": "macos", "Windows": "windows"}.get(platform.system(), "linux")
            arch = "arm64" if platform.machine().lower() in ("aarch64", "arm64") else "x86_64"
            (package / "manifest.xcdn").write_text(f'''#astools_tool {{
              manifest_version:1, id:"wire", version:"1.0.0", title:"Wire fixture",
              summary:"Local contract fixture", kind:"executable", platforms:["{os_name}"],
              runtime:{{mode:"oneshot",entry:[{{os:"{os_name}",arch:"{arch}",argv:[{json.dumps(sys.argv[2])},"echo"]}}]}},
              permissions:{{fs:[],net:false,proc:false,env:[]}},
              commands:[#command {{name:"run",summary:"Observe a fixture",annotations:{{read_only:true,idempotent:true}},
                params:[#param {{name:"msg",type:#type {{kind:"string"}},required:true}}]}}]
            }}''', encoding="utf-8")
            tool_config = root / "astools.xcdn"
            tool_config.write_text("#astools_config " + json.dumps({
                "registry": {"paths": [{"path": str(registry), "trust": "full"}], "watch": "off", "pinning": "off"},
                "workspace": {"root": str(workspace)}, "sandbox": {"default_level": "none"}}), encoding="utf-8")
            config = root / "engine.xcdn"
            config.write_text("#asngn_config " + json.dumps({
                "models": {"pool": [{"id": "remote", "backend": "openai", "provider": provider,
                    "base_url": f"http://127.0.0.1:{server.server_port}/v1", "model": "wire", "ctx": 65536, "warm": False}],
                    "roles": {v: "remote" for v in ("router", "generator", "planner", "compressor", "adapter", "judge")}},
                "routing": {"classifier": "model", "native_actions": True}, "cache": {"enable": False},
                "validation": {"judge": "off"}, "safety": {"max_steps": 4},
                "integration": {"asper": {"enable": False}, "astools": {"enable": True,
                    "root": str(registry), "config": str(tool_config), "workspace": str(workspace)}}}), encoding="utf-8")
            messages = [dict(jsonrpc="2.0", id=1, method="initialize", params={"protocolVersion": "2025-06-18",
                "capabilities": {}, "clientInfo": {"name": "native-wire", "version": "1"}}),
                dict(jsonrpc="2.0", id=2, method="tools/call", params={"name": "agent_ask", "arguments": {"message": "Inspect wire.run"}})]
            proc = subprocess.run([sys.argv[1], "--root", str(root / "store"), "--config", str(config)],
                input="".join(json.dumps(v) + "\n" for v in messages), text=True, capture_output=True, timeout=30)
            assert not server.errors, server.errors
            assert proc.returncode == 0, proc.stderr
            replies = [json.loads(v) for v in proc.stdout.splitlines()]
            result = replies[-1]["result"]
            assert result["isError"] is False, result
            assert "Observed the tool result" in result["content"][0]["text"], result
            assert len(server.requests) == (3 if direct else 4), server.requests
        finally:
            server.shutdown()
            thread.join(timeout=5)


for profile in ("llama-server", "lmstudio"):
    for direct in (False, True):
        exercise(profile, direct)
