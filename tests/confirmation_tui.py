"""The approval pane must reach complete arguments beyond the telemetry preview."""
import subprocess
import sys

result = subprocess.run([sys.argv[1], "--frame-dump-confirm"], capture_output=True,
                        text=True, encoding="utf-8", check=True, timeout=10)
assert "PAYLOAD_END" in result.stdout, result.stdout
assert "Review line 119" in result.stdout, result.stdout
assert "Review line 000" not in result.stdout, result.stdout
assert "edit@1.0.0.patch" in result.stdout, result.stdout
assert "a allow package for session" in result.stdout, result.stdout
