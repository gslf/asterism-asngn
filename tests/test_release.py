"""Exercise release admission against actual clean, dirty and divergent Git trees."""
import copy
import hashlib
import importlib.util
import json
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

spec = importlib.util.spec_from_file_location("release", Path(__file__).resolve().parents[1] / "scripts/release.py")
release = importlib.util.module_from_spec(spec)
spec.loader.exec_module(release)


def git(path, *args):
    return subprocess.check_output(["git", "-C", str(path), "-c", "user.name=Release fixture",
        "-c", "user.email=fixture@example.invalid", *args], text=True, stderr=subprocess.PIPE).strip()


def initialize(path):
    path.mkdir(parents=True)
    git(path, "init", "-q")
    (path / "include").mkdir()
    (path / "include/api.h").write_text("/* fixture public contract */\n")
    git(path, "add", "include")


class ReleaseAdmission(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.storage = tempfile.TemporaryDirectory(prefix="asterism-release-fixture-")
        base = Path(cls.storage.name)
        backend = base / "backend"
        initialize(backend); git(backend, "commit", "-qm", "fixture")
        revision = git(backend, "rev-parse", "HEAD")
        cls.template = base / "family"
        cls.manifest = {"schema": 1, "components": {}, "xcdn_revision": revision, "llama_revision": revision}
        for name in ("asmodel", "asper", "astools", "asngn"):
            path = cls.template / ("asterism-" + name)
            initialize(path)
            if name in ("asper", "astools"):
                (path / "deps").mkdir()
                git(path, "clone", "-q", "--no-local", str(backend), "deps/xcdn-c")
                git(path, "update-index", "--add", "--cacheinfo", "160000," + revision + ",deps/xcdn-c")
            if name == "asper":
                dependency = cls.manifest["components"]["asmodel"]
                (path / "dependencies.json").write_text(json.dumps({"schema": 1, "asmodel": {
                    key: dependency[key] for key in ("repository", "revision")}}))
                git(path, "add", "dependencies.json")
                git(path, "update-index", "--add", "--cacheinfo", "160000," + revision + ",deps/llama.cpp")
            git(path, "commit", "-qm", "fixture")
            cls.manifest["components"][name] = {"repository": "gslf/asterism-" + name,
                "revision": "checkout" if name == "asngn" else git(path, "rev-parse", "HEAD"),
                "headers": {"include/api.h": hashlib.sha256((path / "include/api.h").read_bytes()).hexdigest()}}

    @classmethod
    def tearDownClass(cls):
        cls.storage.cleanup()

    def setUp(self):
        storage = tempfile.TemporaryDirectory(prefix="asterism-release-case-")
        self.addCleanup(storage.cleanup)
        self.root = Path(storage.name) / "family"
        shutil.copytree(self.template, self.root)
        self.manifest = copy.deepcopy(self.__class__.manifest)

    def test_clean_combination_and_explicit_native_requirement(self):
        result = release.verify(self.root, self.manifest)
        self.assertTrue(result["submodules"]["asper/xcdn"]["initialized"])
        self.assertFalse(result["submodules"]["asper/llama"]["initialized"])
        with self.assertRaisesRegex(ValueError, "not initialized"):
            release.verify(self.root, self.manifest, with_llama=True)
        shutil.copytree(self.root / "asterism-asper/deps/xcdn-c", self.root / "asterism-asper/deps/llama.cpp")
        self.assertTrue(release.verify(self.root, self.manifest, with_llama=True)["submodules"]["asper/llama"]["initialized"])

    def test_engine_dirty_opt_in_does_not_hide_contract_drift(self):
        path = self.root / "asterism-asngn"
        (path / "local.txt").write_text("unpublished")
        with self.assertRaisesRegex(ValueError, "dirty source"):
            release.verify(self.root, self.manifest)
        self.assertTrue(release.verify(self.root, self.manifest, True)["components"]["asngn"]["dirty"])
        (path / "include/api.h").write_text("changed ABI")
        with self.assertRaisesRegex(ValueError, "public contract"):
            release.verify(self.root, self.manifest, True)

    def test_standalone_dependency_drift_is_rejected_after_its_commit(self):
        path = self.root / "asterism-asper"
        data = json.loads((path / "dependencies.json").read_text())
        data["asmodel"]["revision"] = "0" * 40
        (path / "dependencies.json").write_text(json.dumps(data))
        git(path, "add", "dependencies.json"); git(path, "commit", "-qm", "diverged dependency")
        self.manifest["components"]["asper"]["revision"] = git(path, "rev-parse", "HEAD")
        with self.assertRaisesRegex(ValueError, "standalone dependency"):
            release.verify(self.root, self.manifest)

    def test_dirty_and_replaced_submodule_checkouts_are_rejected(self):
        path = self.root / "asterism-astools/deps/xcdn-c"
        (path / "include/api.h").write_text("different dependency code")
        with self.assertRaisesRegex(ValueError, "submodule checkout"):
            release.verify(self.root, self.manifest)
        git(path, "add", "include/api.h"); git(path, "commit", "-qm", "other revision")
        with self.assertRaisesRegex(ValueError, "submodule checkout"):
            release.verify(self.root, self.manifest)

    def test_uninitialized_required_submodule_cannot_fall_back_to_parent_git(self):
        shutil.rmtree(self.root / "asterism-asper/deps/xcdn-c/.git")
        with self.assertRaisesRegex(ValueError, "not initialized"):
            release.verify(self.root, self.manifest)

    def test_sibling_dirty_cannot_use_engine_exception(self):
        (self.root / "asterism-asmodel/extra.c").write_text("unpublished dependency")
        with self.assertRaisesRegex(ValueError, "dirty source"):
            release.verify(self.root, self.manifest, True)


if __name__ == "__main__":
    unittest.main()
