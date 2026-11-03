#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Build policy and Linux lifecycle checks; real native/JAR verification belongs to build.sh."""

import contextlib
import hashlib
import importlib.util
import io
import json
from pathlib import Path
import platform
import signal
import subprocess
import tarfile
import tempfile
import unittest
from unittest import mock
import xml.etree.ElementTree as ET
import zipfile


SPEC = importlib.util.spec_from_file_location(
    "cloudbuild_under_test", Path(__file__).with_name("build.py"))
BUILD = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(BUILD)


def publication_fixtures():
    return {
        BUILD.BUNDLE: b"previous successful build",
        BUILD.LEGACY_BUNDLE: b"legacy successful build",
        "SHA256SUMS": b"previous checksum",
        "convai-ame-java-3.0.0-rc.2.jar": b"current JAR",
        "convai-ame-java-3.0.0-rc.2.pom": b"current POM",
        "convai-ame-java-2.0.0.jar": b"older JAR",
        "convai-ame-java-2.0.0.pom": b"older POM",
    }


UNRELATED_FILES = {
    "keep.txt": b"unrelated file",
    "other-sdk-1.0.jar": b"unrelated JAR",
    "convai-ame-java-notes.txt": b"unrelated notes",
}


class GlibcPolicyTest(unittest.TestCase):
    def test_unset_or_empty_limit_accepts_actual_toolchain_requirement(self):
        for env in ({}, {"AME_MAX_GLIBC": ""}):
            with self.subTest(env=env):
                ceiling = BUILD.configured_glibc_limit(env)
                self.assertIsNone(ceiling)
                self.assertEqual(BUILD.required_glibc("GLIBC_2.2.5 GLIBC_2.40 GLIBC_2.9", ceiling), "2.40")

    def test_older_toolchain_does_not_require_glibc_236(self):
        self.assertEqual(BUILD.required_glibc("GLIBC_2.2.5 GLIBC_2.17", None), "2.17")

    def test_explicit_limit_rejects_newer_symbol_requirement(self):
        ceiling = BUILD.configured_glibc_limit({"AME_MAX_GLIBC": "2.17"})
        with self.assertRaisesRegex(ValueError, "requires glibc 2.34; configured maximum is 2.17"):
            BUILD.required_glibc("GLIBC_2.17 GLIBC_2.34", ceiling)

    def test_explicit_limit_accepts_equal_or_older_requirement(self):
        for required in ("2.17", "2.34", "2.34.0"):
            with self.subTest(required=required):
                self.assertEqual(BUILD.required_glibc("GLIBC_" + required, "2.34"), required)

    def test_patch_version_limit_is_compared_numerically(self):
        ceiling = BUILD.configured_glibc_limit({"AME_MAX_GLIBC": "2.2.5"})
        self.assertEqual(BUILD.required_glibc("GLIBC_2.2.5", ceiling), "2.2.5")
        with self.assertRaisesRegex(ValueError, "configured maximum is 2.2.5"):
            BUILD.required_glibc("GLIBC_2.3", ceiling)

    def test_invalid_limit_is_rejected_before_native_build(self):
        for value in ("2", "3.0", "2.36.0.0", "invalid", "2.36 extra"):
            with self.subTest(value=value):
                with self.assertRaisesRegex(ValueError, "AME_MAX_GLIBC"):
                    BUILD.configured_glibc_limit({"AME_MAX_GLIBC": value})

    def test_missing_glibc_metadata_is_not_treated_as_portability(self):
        with self.assertRaisesRegex(ValueError, "Could not determine required glibc"):
            BUILD.required_glibc("GLIBCXX_3.4.29", None)


class MavenPackagingTest(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="ame-maven-package-test-")
        self.addCleanup(temporary.cleanup)
        self.base = Path(temporary.name)
        self.run = self.base / "run"
        self.artifacts = self.base / "artifacts"
        self.run.mkdir()
        self.artifacts.mkdir()
        self.build = BUILD.Build(self.run, self.artifacts)
        self.build.env = {"PATH": "", "AME_MAVEN_OFFLINE": "false"}
        self.source_pom = self.base / "pom.xml"
        project_pom = Path(__file__).resolve().parent.parent / "java/pom.xml"
        self.source_pom.write_bytes(project_pom.read_bytes())
        with mock.patch.object(self.build, "record_git_revision"):
            self.version = self.build.read_project(self.source_pom)
        self.stem = "convai-ame-java-" + self.version
        self.native = self.run / "native"
        self.library = self.native / "native-build/libame_jni.so"
        self.library.parent.mkdir(parents=True)
        # Synthetic native bytes exercise packaging only; CI verifies the real ELF/JNI.
        self.library.write_bytes(b"\x7fELF native packaging fixture")
        self.jar = self.run / (self.stem + ".jar")
        self.properties = ("# Maven packaging fixture\n"
                           "groupId=com.huawei.convai\n"
                           "artifactId=convai-ame-java\n"
                           "version=" + self.version + "\n").encode("utf-8")
        self.entries = {
            "com/huawei/convai/ame/AmeEngine.class": b"Java packaging fixture",
            "com/huawei/convai/ame/NativeLibraryLoader.class": b"Java loader fixture",
            BUILD.NATIVE_ENTRY: self.library.read_bytes(),
            BUILD.POM_ENTRY: self.source_pom.read_bytes(),
            BUILD.PROPERTIES_ENTRY: self.properties,
        }

    def write_jar(self, entries):
        with zipfile.ZipFile(str(self.jar), "w") as archive:
            for name, contents in entries.items():
                archive.writestr(name, contents)

    def verify_entries(self, entries):
        self.write_jar(entries)
        self.build.verify_jar(self.jar, self.library, self.source_pom)

    def test_main_artifact_uses_new_coordinates_without_classifier(self):
        self.assertEqual(self.build.status["group_id"], "com.huawei.convai")
        self.assertEqual(self.build.status["artifact_id"], "convai-ame-java")
        self.assertIsNone(self.build.status["classifier"])
        self.assertEqual(self.build.status["platform"], "linux-x86_64")
        self.assertEqual(self.build.status["java_package"], "com.huawei.convai.ame")
        self.assertEqual(BUILD.NATIVE_ENTRY, "com/huawei/convai/ame/native/linux-x86_64/libame_jni.so")
        self.assertEqual(BUILD.POM_ENTRY, "META-INF/maven/com.huawei.convai/convai-ame-java/pom.xml")
        self.assertEqual(BUILD.PROPERTIES_ENTRY, "META-INF/maven/com.huawei.convai/convai-ame-java/pom.properties")
        ns = {"m": "http://maven.apache.org/POM/4.0.0"}
        project = ET.parse(str(self.source_pom)).getroot()
        self.assertEqual(project.findtext("m:version", namespaces=ns), self.version)
        self.assertEqual(project.findtext("m:packaging", default="jar", namespaces=ns), "jar")
        classifier = project.find(
            "m:build/m:plugins/m:plugin[m:artifactId='maven-jar-plugin']/m:configuration/m:classifier",
            ns)
        self.assertIsNone(classifier)
        sdk = self.run / "java"
        with mock.patch.object(self.build, "execute"):
            jar = self.build.package_java(sdk, self.native, self.version)
        self.assertEqual(jar, sdk / "target" / (self.stem + ".jar"))
        self.verify_entries(self.entries)

    def test_missing_sdk_classes_are_rejected(self):
        for name in ("AmeEngine.class", "NativeLibraryLoader.class"):
            with self.subTest(entry=name):
                entries = dict(self.entries)
                del entries["com/huawei/convai/ame/" + name]
                with self.assertRaisesRegex(ValueError, "Missing SDK class"):
                    self.verify_entries(entries)

    def test_legacy_package_entries_are_rejected(self):
        for name in ("io/ame/AmeEngine.class", "io/ame/native/README.txt"):
            with self.subTest(entry=name):
                entries = dict(self.entries)
                entries[name] = b"legacy fixture"
                with self.assertRaisesRegex(ValueError, "Legacy Java package remains"):
                    self.verify_entries(entries)

    def test_missing_maven_metadata_is_rejected(self):
        for name in (BUILD.POM_ENTRY, BUILD.PROPERTIES_ENTRY):
            with self.subTest(entry=name):
                entries = dict(self.entries)
                del entries[name]
                with self.assertRaisesRegex(ValueError, "Missing Maven metadata"):
                    self.verify_entries(entries)

    def test_wrong_missing_or_duplicate_maven_properties_are_rejected(self):
        changes = (
            (b"groupId=com.huawei.convai", b"groupId=io.ame", "groupId"),
            (b"artifactId=convai-ame-java", b"artifactId=ame-java", "artifactId"),
            (("version=" + self.version).encode(), b"version=0.0.0", "version"),
            (("version=" + self.version + "\n").encode(), b"", "version"),
            (b"groupId=com.huawei.convai", b"groupId=com.huawei.convai\ngroupId=com.huawei.convai", "groupId"),
        )
        for old, new, key in changes:
            with self.subTest(property=key, value=new):
                entries = dict(self.entries)
                entries[BUILD.PROPERTIES_ENTRY] = self.properties.replace(old, new)
                with self.assertRaisesRegex(ValueError, "Unexpected Maven property: " + key):
                    self.verify_entries(entries)

    def test_embedded_pom_must_match_source_bytes(self):
        entries = dict(self.entries)
        entries[BUILD.POM_ENTRY] += b"\n"
        with self.assertRaisesRegex(ValueError, "Embedded POM differs from source POM"):
            self.verify_entries(entries)

    def test_missing_native_is_rejected(self):
        entries = dict(self.entries)
        del entries[BUILD.NATIVE_ENTRY]
        with self.assertRaisesRegex(ValueError, "Unexpected native libraries"):
            self.verify_entries(entries)

    def test_extra_native_libraries_are_rejected(self):
        for name in ("native/other.so", "native/ame_jni.dll", "native/libame_jni.dylib"):
            with self.subTest(entry=name):
                entries = dict(self.entries)
                entries[name] = b"unwanted native library"
                with self.assertRaisesRegex(ValueError, "Unexpected native libraries"):
                    self.verify_entries(entries)

    def test_packaged_native_must_match_fresh_build(self):
        entries = dict(self.entries)
        entries[BUILD.NATIVE_ENTRY] = b"stale native library"
        with self.assertRaisesRegex(ValueError, "Packaged JNI differs from the fresh build"):
            self.verify_entries(entries)

    def prepare_package(self):
        self.verify_entries(self.entries)
        for name in BUILD.NATIVE_METADATA:
            (self.native / name).write_text("native metadata fixture\n", encoding="utf-8")
        for name in BUILD.TOOL_LOGS + BUILD.SDK_TESTS:
            (self.build.logs / (name + ".log")).write_text("test log fixture\n", encoding="utf-8")
        return self.build.prepare_package(
            self.jar, self.library, self.source_pom, self.native, self.version)

    def assert_checksums(self, files):
        lines = files["SHA256SUMS"].decode("utf-8").splitlines()
        entries = [line.split("  ", 1) for line in lines]
        checksums = {name: checksum for checksum, name in entries}
        self.assertEqual(len(checksums), len(lines))
        expected = {name: hashlib.sha256(data).hexdigest()
                    for name, data in files.items() if name != "SHA256SUMS"}
        self.assertEqual(checksums, expected)

    def test_bundle_and_direct_artifacts_match_and_have_complete_checksums(self):
        package = self.prepare_package()
        self.build.publish(package)
        files = {path.name: path.read_bytes() for path in self.artifacts.iterdir()}
        jar_name, pom_name = self.stem + ".jar", self.stem + ".pom"
        self.assertEqual(set(files), {jar_name, pom_name, "convai-ame-java-linux-x86_64.tar.gz", "SHA256SUMS"})
        self.assertEqual(files[jar_name], self.jar.read_bytes())
        self.assertEqual(files[pom_name], self.source_pom.read_bytes())
        self.assert_checksums(files)
        with tarfile.open(str(self.artifacts / BUILD.BUNDLE), "r:gz") as archive:
            members = archive.getnames()
            bundled = {name: archive.extractfile(name).read() for name in members}
        self.assertEqual(len(members), len(bundled))
        self.assertEqual(bundled[jar_name], files[jar_name])
        self.assertEqual(bundled[pom_name], files[pom_name])
        self.assertEqual(bundled["libame_jni.so"], self.library.read_bytes())
        self.assert_checksums(bundled)
        metadata = json.loads(bundled["metadata/build.json"])
        self.assertEqual(metadata["group_id"], "com.huawei.convai")
        self.assertEqual(metadata["artifact_id"], "convai-ame-java")
        self.assertEqual(metadata["version"], self.version)
        self.assertEqual(metadata["java_package"], "com.huawei.convai.ame")
        self.assertIsNone(metadata["classifier"])
        self.assertEqual(metadata["jar_sha256"], hashlib.sha256(files[jar_name]).hexdigest())
        self.assertEqual(metadata["native_sha256"], hashlib.sha256(self.library.read_bytes()).hexdigest())
        self.assertEqual(self.build.status["status"], "VERIFIED")

    def test_cleanup_removes_owned_versions_and_legacy_bundle_only(self):
        fixtures = dict(publication_fixtures(), **UNRELATED_FILES)
        for name, contents in fixtures.items():
            (self.artifacts / name).write_bytes(contents)
        BUILD.clear_artifacts(self.artifacts)
        remaining = {path.name: path.read_bytes() for path in self.artifacts.iterdir()}
        self.assertEqual(remaining, UNRELATED_FILES)


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
        files = dict(publication_fixtures(), **UNRELATED_FILES)
        for name, data in files.items():
            (self.artifacts / name).write_bytes(data)
        return files

    def test_failed_retry_clears_old_publication_and_preserves_diagnostics(self):
        self.seed_published_files()

        def fail_compilation(build):
            self.assertEqual({path.name for path in self.artifacts.iterdir()}, set(UNRELATED_FILES))
            build.status["step"] = "native-build"
            (build.logs / "native-build.log").write_text("compiler failed\n", encoding="utf-8")
            raise subprocess.CalledProcessError(17, "native-build")

        with mock.patch.object(BUILD.Build, "build", fail_compilation):
            with self.assertRaises(subprocess.CalledProcessError) as failure:
                self.run_main()
        self.assertEqual(failure.exception.returncode, 17)
        remaining = {path.name: path.read_bytes() for path in self.artifacts.iterdir()}
        self.assertEqual(remaining, UNRELATED_FILES)
        statuses = list((self.output / "runs").glob("*/status.json"))
        self.assertEqual(len(statuses), 1)
        status = json.loads(statuses[0].read_text(encoding="utf-8"))
        self.assertEqual(status["status"], "FAILED")
        self.assertEqual(status["step"], "native-build")
        self.assertEqual((statuses[0].parent / "logs/native-build.log").read_text(encoding="utf-8"),
                         "compiler failed\n")

    def test_failed_publication_clears_partial_artifacts(self):
        self.seed_published_files()

        def fail_publication(build):
            build.status["step"] = "package-artifacts"
            for name, contents in publication_fixtures().items():
                (self.artifacts / name).write_bytes(contents)
            raise OSError("artifact copy failed")

        with mock.patch.object(BUILD.Build, "build", fail_publication):
            with self.assertRaisesRegex(OSError, "artifact copy failed"):
                self.run_main()
        remaining = {path.name: path.read_bytes() for path in self.artifacts.iterdir()}
        self.assertEqual(remaining, UNRELATED_FILES)
        statuses = list((self.output / "runs").glob("*/status.json"))
        self.assertEqual(len(statuses), 1)
        status = json.loads(statuses[0].read_text(encoding="utf-8"))
        self.assertEqual(status["status"], "FAILED")
        self.assertEqual(status["step"], "package-artifacts")

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
