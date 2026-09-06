"""Relocate an actual installed runtime; exercise diagnostics and packaged strict tools."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
from runtime_archive import extract


def check(prefix):
    env = {key: value for key, value in os.environ.items() if key not in {"ASTOOLS_PATH", "ASTOOLS_JAIL"}}
    with tempfile.TemporaryDirectory(prefix="asterism-installed-") as directory:
        work = Path(directory)
        root = work / "absent-engine"
        doctor = subprocess.run([prefix / "bin/asngn", "--root", root, "--doctor"],
            cwd=work, env=env, text=True, capture_output=True, timeout=15)
        assert doctor.returncode != 0 and "Preflight: action required" in doctor.stdout, doctor
        assert "Embedded backend absent" in doctor.stdout and not root.exists(), doctor
        version = subprocess.run([prefix / "bin/asngn", "--version"], cwd=work,
            env=env, text=True, capture_output=True, check=True, timeout=15)
        assert "0." in version.stdout, version
        for name in ("asngn", "asper", "asmodel", "astools", "xcdn"):
            assert (prefix / "share/asterism/licenses" / (name + ".txt")).is_file()
        packages = prefix / "share/asterism/tools"
        for package in packages.iterdir():
            subprocess.run([prefix / "bin/astools-check", package], check=True,
                env=env, cwd=work, capture_output=True, timeout=15)
        sample = work / "sample.txt"; sample.write_text("installed tools: tè 🍵\n", encoding="utf-8")
        config = work / "astools.xcdn"
        config.write_text("#astools_config " + json.dumps({
            "registry": {"paths": [{"path": str(packages), "trust": "standard"}], "watch": "off", "pinning": "off"},
            "sandbox": {"default_level": "strict", "strict_fallback": "reject"},
            "workspace": {"root": str(work)}}))
        meta = {"io.modelcontextprotocol/protocolVersion": "2026-07-28",
                "io.modelcontextprotocol/clientCapabilities": {}}
        request = {"jsonrpc": "2.0", "id": 1, "method": "tools/call", "params": {
            "_meta": meta, "name": "fs_read", "arguments": {"path": str(sample)}}}
        run = subprocess.run([prefix / "bin/astools-mcp", "--config", config], env=env,
            cwd=work, input=json.dumps(request)+"\n", text=True, capture_output=True, timeout=20)
        assert run.returncode == 0, run.stderr
        result = json.loads(run.stdout)["result"]
        assert not result["isError"] and "installed tools" in result["content"][0]["text"], result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--build", type=Path)
    mode.add_argument("--archive", type=Path)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="asterism-install-") as directory:
        if args.archive:
            check(extract(args.archive, Path(directory) / "extracted"))
            print("Runtime archive: extracted payload passed installed-runtime checks")
            return
        prefix = Path(directory) / "original"
        subprocess.run(["cmake", "--install", args.build, "--prefix", prefix],
            check=True, capture_output=True, timeout=30)
        relocated = prefix.with_name("relocated runtime"); prefix.rename(relocated)
        check(relocated)
    print("Installed runtime: relocation, read-only doctor, packaged contracts and strict tool execution passed")


if __name__ == "__main__":
    main()
