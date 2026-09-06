"""Build and install both archives away from the checkout, then use their APIs."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import zipfile

sdk = Path(__file__).resolve().parents[1]
engine, node, tsc = map(lambda value: str(Path(value).resolve()), sys.argv[1:4])


def run(argv, cwd):
    result = subprocess.run(list(map(str, argv)), cwd=cwd, text=True,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=90)
    if result.returncode:
        raise RuntimeError(f"{argv[0]} exited {result.returncode}:\n{result.stdout}")


with tempfile.TemporaryDirectory(prefix="asngn-sdk-packages-") as temp:
    root = Path(temp)
    source, consumer = root / "source", root / "consumer"
    consumer.mkdir()
    shutil.copytree(sdk / "python", source, ignore=shutil.ignore_patterns("build", "*.egg-info", "__pycache__"))
    run([sys.executable, "-m", "pip", "wheel", "--no-index", "--no-build-isolation", "--no-cache-dir",
         "--no-deps", "--wheel-dir", root, source], root)
    wheel, = root.glob("*.whl")
    with zipfile.ZipFile(wheel) as archive:
        assert "asterism/py.typed" in archive.namelist()
        metadata, = [name for name in archive.namelist() if name.endswith(".dist-info/METADATA")]
        assert "Requires-Dist:" not in archive.read(metadata).decode()
        assert any(name.endswith("licenses/LICENSE") for name in archive.namelist())
    run([sys.executable, "-m", "pip", "install", "--no-index", "--no-deps", "--no-cache-dir",
         "--target", consumer / "python", wheel], root)
    python = consumer / "consumer.py"
    python.write_text('''import asyncio, sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).with_name("python")))
import asterism
assert Path(asterism.__file__).is_relative_to(Path(__file__).parent)
async def main():
    async with await asterism.Client.start(sys.argv[1:]) as client:
        assert (await client.session().approval())["status"] == "none"
asyncio.run(main())
''', encoding="utf-8")
    run([sys.executable, "-I", python, engine, "--root", root / "python-store"], consumer)
    # Invoke npm through its JS entry point on Windows too; never enable a shell.
    npm_command = shutil.which("npm")
    if not npm_command:
        raise RuntimeError("npm is required for packaging checks")
    npm_root = Path(npm_command).resolve().parent
    npm_cli = npm_root / ("node_modules/npm/bin/npm-cli.js" if os.name == "nt" else "npm-cli.js")
    if not npm_cli.is_file():
        npm_cli = Path(node).parent / "node_modules/npm/bin/npm-cli.js"
    npm = [node, npm_cli] if npm_cli.is_file() else [npm_command]
    run([*npm, "pack", sdk / "javascript", "--cache", root / "npm-cache", "--ignore-scripts", "--pack-destination", root], root)
    package, = root.glob("*.tgz")
    run([*npm, "install", "--offline", "--ignore-scripts", "--no-audit", "--no-fund", "--cache", root / "npm-cache", package], consumer)
    javascript = consumer / "consumer.mjs"
    javascript.write_text('''import assert from 'node:assert/strict';
import { Client } from '@asterism/sdk';
const client = await Client.start(process.argv.slice(2));
try { assert.equal((await client.session().approval()).status, 'none'); }
finally { await client.close(); }
''', encoding="utf-8")
    run([node, javascript, engine, "--root", root / "javascript-store"], consumer)
    types = consumer / "consumer.mts"
    types.write_text((sdk / "tests/types.mts").read_text().replace("../javascript/src/index.mjs", "@asterism/sdk"), encoding="utf-8")
    run([node, tsc, "--noEmit", "--strict", "--module", "nodenext", "--target", "es2022",
         "--lib", "es2022,dom,esnext.disposable", types], consumer)
    print(json.dumps({"status": "passed", "wheel": wheel.name, "npm": package.name,
                      "checks": ["license", "runtime_dependencies", "isolated_imports", "live_mcp", "installed_types"]}))
