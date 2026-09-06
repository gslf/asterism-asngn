#!/usr/bin/env python3
"""Build an unsigned Linux runtime from clean local pins and test its relocation."""
import argparse
import hashlib
import json
from pathlib import Path
import platform
import shutil
import subprocess
import sys
import tempfile

from release import verify


def package(root, output, jobs):
    root = root.resolve(strict=True)
    if platform.system() != "Linux":
        raise ValueError("The validated runtime distribution profile currently requires Linux")
    manifest = json.loads((root / "asterism-asngn/release.json").read_text())
    resolved = verify(root, manifest)
    output = output.absolute()
    if output.exists() or output.is_symlink():
        raise FileExistsError("Choose a new output directory; existing artifacts are never overwritten")
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="asterism-package-") as directory:
        stage = Path(directory)
        log = stage / "build-test.log"

        def run(*args):
            with log.open("a") as stream:
                print(json.dumps(list(map(str, args))), file=stream, flush=True)
                result = subprocess.run(list(map(str, args)), stdout=stream, stderr=subprocess.STDOUT)
            if result.returncode:
                tail = log.read_text(errors="replace")[-8192:]
                raise ValueError(f"Build command exited {result.returncode}:\n{tail}")

        for name in ("asngn", "asmodel", "asper", "astools"):
            source = root / ("asterism-" + name)
            target = stage / source.name
            run("git", "clone", "--no-local", "--no-checkout", source, target)
            run("git", "-C", target, "checkout", "--detach", resolved["components"][name]["revision"])
        for name in ("asper", "astools"):
            target = stage / ("asterism-" + name) / "deps/xcdn-c"
            run("git", "clone", "--no-local", "--no-checkout", root / "asterism-astools/deps/xcdn-c", target)
            run("git", "-C", target, "checkout", "--detach", manifest["xcdn_revision"])
        isolated = verify(stage, manifest)
        build = stage / "build"
        run("cmake", "-S", stage / "asterism-asngn", "-B", build,
            "-DCMAKE_BUILD_TYPE=Release", "-DASNGN_WITH_LLAMA=OFF", "-DASNGN_BUILD_DISTRIBUTION=ON",
            "-DPython3_EXECUTABLE=" + sys.executable)
        run("cmake", "--build", build, "--parallel", jobs)
        run("ctest", "--test-dir", build, "-R", "^test_distribution$", "--no-tests=error",
            "--output-on-failure", "--output-junit", stage / "installation-test.xml")
        run("cmake", "--build", build, "--target", "package")
        archives = list(build.glob("asterism-*.tar.gz"))
        if len(archives) != 1:
            raise ValueError("CPack must produce exactly one runtime archive")
        run(sys.executable, stage / "asterism-asngn/tests/distribution.py", "--archive", archives[0])
        with archives[0].open("rb") as stream:
            digest = hashlib.file_digest(stream, "sha256").hexdigest()
        checksum = archives[0].with_suffix(archives[0].suffix + ".sha256")
        if checksum.read_text().strip() != digest + "  " + archives[0].name:
            raise ValueError("CPack archive checksum differs from the actual artifact")
        # Revalidate the isolated source after building; never inherit a dirty release claim.
        if verify(stage, manifest) != isolated:
            raise ValueError("Source identity changed during packaging")
        receipt = {"schema": 1, "profile": "linux-remote", "signed": False,
            "archive": {"name": archives[0].name, "sha256": digest, "bytes": archives[0].stat().st_size},
            "sources": isolated, "platform": platform.platform(), "machine": platform.machine(),
            "validation": ["relocated runtime", "ACP v1 initialization", "doctor without inference", "strict packaged tool"],
            "not_validated": ["model inference", "other distributions", "full provider conformance"]}
        (stage / "build-receipt.json").write_text(json.dumps(receipt, indent=2) + "\n")
        files = [archives[0], checksum,
                 log, stage / "installation-test.xml", stage / "build-receipt.json"]
        # Output is reserved only after all required artifacts and checks exist.
        if not all(path.is_file() for path in files):
            raise ValueError("A required package artifact is missing")
        output.mkdir()
        try:
            for path in files:
                shutil.copyfile(path, output / path.name)
        except BaseException:
            shutil.rmtree(output)
            raise
    return output


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[2])
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--jobs", type=int, default=4)
    args = parser.parse_args()
    if not 1 <= args.jobs <= 32:
        parser.error("--jobs must be between 1 and 32")
    try:
        print(package(args.root, args.output, args.jobs))
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        parser.exit(1, f"Package failed: {error}\n")


if __name__ == "__main__":
    main()
