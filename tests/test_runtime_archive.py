"""Malformed runtime archives must fail before creating their destination."""
import io
from pathlib import Path
import sys
import tarfile
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
from runtime_archive import extract


class RuntimeArchive(unittest.TestCase):
    def test_unsafe_entries_and_incomplete_layout_do_not_extract(self):
        cases = [
            [("../escape", tarfile.REGTYPE, 0o644)],
            [("/absolute", tarfile.REGTYPE, 0o644)],
            [("C:escape", tarfile.REGTYPE, 0o644)],
            [("one/link", tarfile.SYMTYPE, 0o644)],
            [("one/hard", tarfile.LNKTYPE, 0o644)],
            [("one/device", tarfile.CHRTYPE, 0o644)],
            [("one/suid", tarfile.REGTYPE, 0o4755)],
            [("one/duplicate", tarfile.REGTYPE, 0o644)] * 2,
            [("one/file", tarfile.REGTYPE, 0o644), ("two/file", tarfile.REGTYPE, 0o644)],
            [("one/only", tarfile.REGTYPE, 0o644)],
        ]
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for i, rows in enumerate(cases):
                with self.subTest(rows=rows):
                    archive = root / f"bad-{i}.tar.gz"
                    with tarfile.open(archive, "w:gz") as stream:
                        for name, kind, mode in rows:
                            info = tarfile.TarInfo(name); info.type = kind; info.mode = mode
                            stream.addfile(info, io.BytesIO())
                    destination = root / f"out-{i}"
                    with self.assertRaises(ValueError):
                        extract(archive, destination)
                    self.assertFalse(destination.exists())

    def test_existing_destination_is_never_reused(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            marker = root / "existing"; marker.write_text("keep")
            with self.assertRaises(FileExistsError):
                extract(root / "absent.tar.gz", root)
            self.assertEqual(marker.read_text(), "keep")


if __name__ == "__main__":
    unittest.main()
