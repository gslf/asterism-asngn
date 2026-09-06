"""Supply a loopback provider to the JavaScript SDK integration."""
import json
from pathlib import Path
import subprocess
import sys
from server_fixture import fixture

with fixture(sys.argv[1]) as (command, server):
    subprocess.run([sys.argv[2], str(Path(__file__).with_suffix(".mjs")), json.dumps(command), str(server.started_path)], check=True, timeout=30)
    assert server.started.is_set(), "cancellation did not reach an actual HTTP request"
    assert len(server.requests) == 2, server.requests
with fixture(sys.argv[1], sys.argv[3]) as (command, server):
    subprocess.run([sys.argv[2], str(Path(__file__).with_name("javascript_approval.mjs")), json.dumps(command)], check=True, timeout=30)
    assert len(server.requests) == 2, server.requests
