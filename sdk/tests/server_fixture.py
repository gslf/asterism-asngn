"""Real engine fixture with deterministic HTTP responses, never a quality eval."""
from contextlib import contextmanager
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import platform
from pathlib import Path
import tempfile
import threading
import time

ANSWER = "Asterism α 日本語 " * 400
REVIEW = "Review α 日本語 " * 400


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *_args):
        pass

    def do_POST(self):
        request = json.loads(self.rfile.read(int(self.headers["content-length"])))
        self.server.requests.append(request)
        # The latest user message follows prior history in the response prompt.
        user = request["messages"][-1]["content"]
        if "sdk-slow" in user:
            self.server.started.set()
            self.server.started_path.touch()
            time.sleep(0.8)
        reply = {"choices": [{"message": {"role": "assistant", "content": ANSWER}, "finish_reason": "stop"}],
                 "usage": {"prompt_tokens": 100, "completion_tokens": 100}}
        if self.server.approval:
            if request.get("tools"):
                tool = next(t["function"] for t in request["tools"] if t["function"]["description"].startswith("wire.mut:"))
                reply["choices"][0] = {"message": {"role": "assistant", "content": None, "tool_calls": [{
                    "id": "approval-proposal", "type": "function", "function": {
                        "name": tool["name"], "arguments": json.dumps({"msg": REVIEW})}}]}, "finish_reason": "tool_calls"}
            else:
                reply["choices"][0]["message"]["content"] = "CLASS MODERATE | DETAIL NORMAL | MODE PLAN | TASK LOOKUP\n"
        body = json.dumps(reply, ensure_ascii=True).encode()
        try:
            self.send_response(200)
            self.send_header("content-type", "application/json")
            self.send_header("content-length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
        except (BrokenPipeError, ConnectionResetError):
            pass  # Cancellation intentionally closes the HTTP socket.


@contextmanager
def fixture(binary, tool_binary=None):
    with tempfile.TemporaryDirectory(prefix="asngn-sdk-") as tmp, ThreadingHTTPServer(("127.0.0.1", 0), Handler) as server:
        root = Path(tmp)
        server.requests, server.started = [], threading.Event()
        server.started_path = root / "http-started"
        server.approval = tool_binary is not None
        config = root / "engine.xcdn"
        settings = {
            "models": {"pool": [{"id": "remote", "backend": "openai", "provider": "llama-server",
                "base_url": f"http://127.0.0.1:{server.server_port}/v1", "model": "sdk-fixture", "ctx": 65536, "warm": False}],
                "roles": {v: "remote" for v in ("router", "generator", "planner", "compressor", "adapter", "judge")}},
            "routing": {"classifier": "heuristic"}, "cache": {"enable": False},
            "validation": {"judge": "off"},
            "integration": {"asper": {"enable": False}, "astools": {"enable": False}}}
        if tool_binary:
            workspace, registry = root / "workspace", root / "registry"
            workspace.mkdir()
            package = registry / "wire"
            package.mkdir(parents=True)
            os_name = {"Darwin": "macos", "Windows": "windows"}.get(platform.system(), "linux")
            arch = "arm64" if platform.machine().lower() in ("aarch64", "arm64") else "x86_64"
            (package / "manifest.xcdn").write_text(f'''#astools_tool {{
                manifest_version:1, id:"wire", version:"1.0.0", title:"SDK fixture",
                summary:"Approval fixture", kind:"executable", platforms:["{os_name}"],
                runtime:{{mode:"oneshot",entry:[{{os:"{os_name}",arch:"{arch}",argv:[{json.dumps(tool_binary)},"echo"]}}]}},
                permissions:{{fs:[],net:false,proc:false,env:[]}},
                commands:[#command {{name:"mut",summary:"Review a mutation",annotations:{{destructive:true}},
                    params:[#param {{name:"msg",type:#type {{kind:"string"}},required:true}}]}}]
            }}''', encoding="utf-8")
            tool_config = root / "astools.xcdn"
            tool_config.write_text("#astools_config " + json.dumps({
                "registry": {"paths": [{"path": str(registry), "trust": "full"}], "watch": "off", "pinning": "off"},
                "workspace": {"root": str(workspace)}, "sandbox": {"default_level": "none"}}), encoding="utf-8")
            settings["routing"] = {"classifier": "model", "native_actions": True}
            settings["mcp"] = {"autoconfirm": "prompt"}
            settings["integration"]["astools"] = {"enable": True, "root": str(registry),
                "config": str(tool_config), "workspace": str(workspace)}
        config.write_text("#asngn_config " + json.dumps(settings), encoding="utf-8")
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        try:
            yield [binary, "--root", str(root / "store"), "--config", str(config)], server
        finally:
            server.shutdown()
            thread.join(timeout=5)
