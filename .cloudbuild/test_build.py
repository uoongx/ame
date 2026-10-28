#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Linux-only lifecycle checks; real native/JAR verification belongs to build.sh."""

import contextlib
import importlib.util
import io
import json
from pathlib import Path
import platform
import signal
import subprocess
import tempfile
import unittest
from unittest import mock


SPEC = importlib.util.spec_from_file_location(
    "cloudbuild_under_test", Path(__file__).with_name("build.py"))
BUILD = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(BUILD)


@unittest.skipUnless(platform.system() == "Linux", "Requires Linux flock and symlink semantics")
class BuildLifecycleTest(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="ame-cloudbuild-test-")
        self.addCleanup(temporary.cleanup)
        self.base = Path(temporary.name).resolve()
        self.root = self.base / "workspace"
        self.root.mkdir()
        self.output = self.root / "build" / "cloudbuild"
        self.artifacts = self.output / "artifacts"
        for name, value in (("ROOT", self.root), ("OUTPUT", self.output)):
            patcher = mock.patch.object(BUILD, name, value)
            patcher.start()
            self.addCleanup(patcher.stop)
        argv = mock.patch("sys.argv", ["build.py"])
        argv.start()
        self.addCleanup(argv.stop)
        for signum in (signal.SIGTERM, signal.SIGINT):
            self.addCleanup(signal.signal, signum, signal.getsignal(signum))

    def run_main(self):
        with contextlib.redirect_stdout(io.StringIO()):
            BUILD.main()

    def seed_published_files(self):
        self.artifacts.mkdir(parents=True)
        files = {
            BUILD.BUNDLE: b"previous successful build",
            "SHA256SUMS": b"previous checksum",
            "keep.txt": b"unrelated file",
        }
        for name, data in files.items():
            (self.artifacts / name).write_bytes(data)
        return files

    def test_failed_retry_clears_old_publication_and_preserves_diagnostics(self):
        self.seed_published_files()

        def fail_compilation(build):
            self.assertFalse((self.artifacts / BUILD.BUNDLE).exists())
            self.assertFalse((self.artifacts / "SHA256SUMS").exists())
            build.status["step"] = "native-build"
            (build.logs / "native-build.log").write_text("compiler failed\n", encoding="utf-8")
            raise subprocess.CalledProcessError(17, "native-build")

        with mock.patch.object(BUILD.Build, "build", fail_compilation):
            with self.assertRaises(subprocess.CalledProcessError) as failure:
                self.run_main()
        self.assertEqual(failure.exception.returncode, 17)
        self.assertFalse((self.artifacts / BUILD.BUNDLE).exists())
        self.assertFalse((self.artifacts / "SHA256SUMS").exists())
        self.assertEqual((self.artifacts / "keep.txt").read_bytes(), b"unrelated file")
        statuses = list((self.output / "runs").glob("*/status.json"))
        self.assertEqual(len(statuses), 1)
        status = json.loads(statuses[0].read_text(encoding="utf-8"))
        self.assertEqual(status["status"], "FAILED")
        self.assertEqual(status["step"], "native-build")
        self.assertEqual((statuses[0].parent / "logs/native-build.log").read_text(encoding="utf-8"),
                         "compiler failed\n")

    def test_lock_conflict_preserves_active_build_artifacts(self):
        import fcntl

        original = self.seed_published_files()
        with (self.output / ".build.lock").open("a") as active_lock:
            fcntl.flock(active_lock.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
            with mock.patch.object(BUILD.Build, "build") as build:
                with self.assertRaisesRegex(ValueError, "Another build"):
                    self.run_main()
                build.assert_not_called()
        for name, data in original.items():
            self.assertEqual((self.artifacts / name).read_bytes(), data)
        self.assertEqual(list((self.output / "runs").iterdir()), [])

    def assert_symlink_rejected(self, link):
        target = self.base / "outside-workspace"
        target.mkdir()
        sentinel = target / BUILD.BUNDLE
        sentinel.write_bytes(b"must remain untouched")
        link.parent.mkdir(parents=True, exist_ok=True)
        link.symlink_to(target, target_is_directory=True)
        with mock.patch.object(BUILD.Build, "build") as build:
            with self.assertRaisesRegex(ValueError, "symlink aliases"):
                self.run_main()
            build.assert_not_called()
        self.assertTrue(link.is_symlink())
        self.assertEqual(sentinel.read_bytes(), b"must remain untouched")
        self.assertEqual(list(target.iterdir()), [sentinel])

    def test_output_symlink_does_not_modify_its_target(self):
        self.assert_symlink_rejected(self.output)

    def test_artifacts_symlink_does_not_modify_its_target(self):
        self.assert_symlink_rejected(self.artifacts)


if __name__ == "__main__":
    unittest.main(verbosity=2)
