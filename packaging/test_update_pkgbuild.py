import hashlib
import importlib.util
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("update_pkgbuild", ROOT / "packaging/update-pkgbuild.py")
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


class UpdatePkgbuildTest(unittest.TestCase):
    def test_metadata_is_pinned_without_enabling_tests(self):
        template = (ROOT / "PKGBUILD").read_text()
        text = MODULE.render(template, "example/snipline", "1.2.3", "a" * 64)
        self.assertIn("pkgver=1.2.3", text)
        self.assertIn("url='https://github.com/example/snipline'", text)
        self.assertIn("sha256sums=('" + "a" * 64 + "')", text)
        self.assertIn("-DBUILD_TESTING=OFF", text)
        self.assertNotIn("check()", text)
        self.assertNotIn("SKIP", text)

    def test_invalid_metadata_is_rejected(self):
        template = (ROOT / "PKGBUILD").read_text()
        for repository, version in [("bad", "1.0.0"), ("a/b;echo bad", "1.0.0"),
                                    ("a/b", "v1.0.0"), ("a/b", "1.0.0;echo bad")]:
            with self.assertRaises(ValueError):
                MODULE.render(template, repository, version, "a" * 64)

    def test_local_archive_hash(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)
            archive = path / "source.tar.gz"
            archive.write_bytes(b"local source fixture")
            output = path / "PKGBUILD"
            subprocess.run([sys.executable, str(ROOT / "packaging/update-pkgbuild.py"),
                            "--repository", "example/snipline", "--version", "1.2.3",
                            "--archive", str(archive), "--output", str(output)], check=True)
            self.assertIn(hashlib.sha256(archive.read_bytes()).hexdigest(), output.read_text())


if __name__ == "__main__":
    unittest.main()
