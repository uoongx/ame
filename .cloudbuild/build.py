#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Build and verify the Linux x86_64 Java SDK. Python 3.8+, standard library only."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import shutil
import signal
import subprocess
import sys
import tarfile
import tempfile
import time
import xml.etree.ElementTree as ET
import zipfile


ROOT = Path(__file__).resolve().parent.parent
OUTPUT = ROOT / "build" / "cloudbuild"
CLASSIFIER = "linux-x86_64"
BUNDLE = "ame-java-linux-x86_64.tar.gz"
NATIVE_ENTRY = "io/ame/native/linux-x86_64/libame_jni.so"
POM_ENTRY = "META-INF/maven/io.ame/ame-java/pom.xml"


def require(condition, message):
    if not condition:
        raise ValueError(message)


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def write_json(path, value):
    path.write_text(json.dumps(value, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")


def clear_artifacts(directory):
    # Only these two pipeline-owned files are replaced, never a recursive workspace cleanup.
    for name in (BUNDLE, "SHA256SUMS"):
        path = directory / name
        require(not path.is_dir(), "Artifact path is a directory: " + str(path))
        if path.exists() or path.is_symlink():
            path.unlink()


def stop(signum, _frame):
    raise InterruptedError("Build interrupted by signal " + str(signum))


class Build:
    def __init__(self, run, artifacts):
        self.run = run
        self.artifacts = artifacts
        self.logs = run / "logs"
        self.logs.mkdir()
        self.env = os.environ.copy()
        self.env.update(LANG="C.UTF-8", LC_ALL="C.UTF-8")
        self.status = {"status": "FAILED", "platform": CLASSIFIER, "step": "preflight"}

    def execute(self, step, command, env=None):
        self.status["step"] = step
        print("AME CloudBuild: " + step, flush=True)
        with (self.logs / (step + ".log")).open("wb") as log:
            process = subprocess.Popen([str(arg) for arg in command], cwd=str(ROOT),
                                       env=self.env if env is None else env,
                                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                       start_new_session=True)
            try:
                for line in iter(process.stdout.readline, b""):
                    log.write(line)
                    sys.stdout.buffer.write(line)
                    sys.stdout.buffer.flush()
                code = process.wait()
                if code:
                    raise subprocess.CalledProcessError(code, step)
            finally:
                if process.poll() is None:
                    os.killpg(process.pid, signal.SIGTERM)
                    try:
                        process.wait(timeout=10)
                    except subprocess.TimeoutExpired:
                        os.killpg(process.pid, signal.SIGKILL)
                        process.wait()

    def build(self):
        require(platform.machine().lower() in ("x86_64", "amd64"), "Only Linux x86_64 is supported")
        java_home = self.env.get("JAVA_HOME")
        if java_home:
            for tool in ("java", "javac"):
                require((Path(java_home) / "bin" / tool).is_file(), "Invalid JAVA_HOME: missing " + tool)
            self.env["PATH"] = str(Path(java_home) / "bin") + os.pathsep + self.env["PATH"]
        for tool in ("sh", "java", "javac", "mvn", "g++", "cmake", "make", "readelf", "nm", "sha256sum", "readlink"):
            require(shutil.which(tool, path=self.env["PATH"]), "Missing build tool: " + tool)
        jobs = self.env.get("AME_BUILD_JOBS", "2")
        require(re.fullmatch(r"[1-9][0-9]*", jobs), "AME_BUILD_JOBS must be a positive integer")
        self.env["AME_BUILD_JOBS"] = jobs
        offline = self.env.get("AME_MAVEN_OFFLINE", "false")
        require(offline in ("true", "false"), "AME_MAVEN_OFFLINE must be true or false")
        max_glibc = self.env.get("AME_MAX_GLIBC", "2.36")
        require(re.fullmatch(r"2\.[0-9]+", max_glibc), "AME_MAX_GLIBC must be 2.<minor>")
        self.execute("cmake-version", ["cmake", "--version"])
        cmake_version = re.search(r"cmake version (\d+)\.(\d+)",
                                  (self.logs / "cmake-version.log").read_text())
        require(cmake_version and tuple(map(int, cmake_version.groups())) >= (3, 24),
                "CMake >= 3.24 is required for headless JNI builds")
        self.execute("maven-version", ["mvn", "--version"])
        for tool in ("java", "javac"):
            self.execute(tool + "-version", [tool, "-version"])
            version_text = (self.logs / (tool + "-version.log")).read_text()
            match = re.search(r'(?:version\s+"|javac\s+)(\d+)', version_text)
            require(match and int(match.group(1)) >= 17, tool + " must be version 17 or newer")

        source_pom = ROOT / "java" / "pom.xml"
        project = ET.parse(str(source_pom)).getroot()
        ns = {"m": "http://maven.apache.org/POM/4.0.0"}
        version = project.findtext("m:version", namespaces=ns)
        require(version and re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9_.+-]*", version), "Invalid SDK version in java/pom.xml")
        require(project.findtext("m:groupId", namespaces=ns) == "io.ame"
                and project.findtext("m:artifactId", namespaces=ns) == "ame-java", "Unexpected Maven coordinates")
        self.status.update(version=version, classifier=CLASSIFIER, java_release=17,
                           max_glibc=max_glibc, run_id=self.run.name)
        if shutil.which("git", path=self.env["PATH"]):
            result = subprocess.run(["git", "-C", str(ROOT), "rev-parse", "HEAD"],
                                    stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, env=self.env)
            self.status["git_commit"] = result.stdout.decode().strip() if result.returncode == 0 else "unavailable"

        # Isolate Maven output and copy only sources. Old target trees and checked-out native
        # binaries never become inputs; CMake writes only to this fresh run directory.
        sdk = self.run / "java"
        sdk.mkdir()
        shutil.copy2(str(source_pom), str(sdk / "pom.xml"))
        for relative in ("src/main/java", "src/test/java", "src/main/resources"):
            source = ROOT / "java" / relative
            if source.exists():
                shutil.copytree(str(source), str(sdk / relative),
                                ignore=shutil.ignore_patterns("*.dll", "*.so", "*.dylib"))
        native = self.run / "native"
        self.execute("native-build", ["sh", ROOT / "java/scripts/build-linux-native.sh", native])
        header = (native / "elf-header.txt").read_text()
        require("ELF64" in header and "Advanced Micro Devices X86-64" in header, "JNI binary is not ELF64 x86_64")
        versions = re.findall(r"GLIBC_(\d+\.\d+(?:\.\d+)?)", (native / "elf-versions.txt").read_text())
        require(versions, "Could not determine required glibc from the native library")
        required = max(versions, key=lambda value: tuple(map(int, value.split("."))))
        require(tuple(map(int, required.split("."))) <= tuple(map(int, max_glibc.split("."))),
                "Native library requires glibc " + required + "; configured maximum is " + max_glibc)
        self.status["required_glibc"] = required
        command = ["mvn", "-B", "-ntp", "-f", sdk / "pom.xml"]
        if offline == "true":
            command.append("-o")
        for variable, argument in (("AME_MAVEN_SETTINGS", "-s"), ("AME_MAVEN_REPO", "-Dmaven.repo.local=")):
            if self.env.get(variable):
                path = Path(self.env[variable])
                require(path.is_absolute(), variable + " must be an absolute path")
                if variable == "AME_MAVEN_SETTINGS":
                    require(path.is_file(), "Maven settings file does not exist")
                    command.extend([argument, path])
                else:
                    command.append(argument + str(path))
        command += ["-Dame.native.resources=" + str(native / "native-build/resources"),
                    "-DskipTests", "package"]
        self.execute("maven-package", command)
        filename = "ame-java-" + version + "-" + CLASSIFIER + ".jar"
        jar = sdk / "target" / filename
        library = native / "native-build/libame_jni.so"
        with zipfile.ZipFile(str(jar)) as archive:
            names = archive.namelist()
            require(len(names) == len(set(names)) and archive.testzip() is None, "Invalid JAR archive")
            libraries = [name for name in names if name.endswith((".so", ".dll", ".dylib"))]
            require(libraries == [NATIVE_ENTRY], "Unexpected native libraries in the JAR: " + str(libraries))
            require(archive.read(NATIVE_ENTRY) == library.read_bytes(), "Packaged JNI differs from the fresh build")
            require(archive.read(POM_ENTRY) == source_pom.read_bytes(), "Embedded POM differs from source POM")

        # These are executable test mains, not JUnit tests. Test the JAR resource loader;
        # target/classes and external native paths must not be used in this classpath.
        test_env = self.env.copy()
        for key in ("JAVA_TOOL_OPTIONS", "JDK_JAVA_OPTIONS", "_JAVA_OPTIONS", "CLASSPATH"):
            test_env.pop(key, None)
        for test in ("NativeLibraryLoaderTest", "AmeEngineSmokeTest", "AmeEngineBoundaryTest"):
            self.execute(test, ["java", "-cp", str(sdk / "target/test-classes") + ":" + str(jar),
                                "io.ame." + test] + ([] if test == "NativeLibraryLoaderTest" else
                                [self.run / "test-data" / test]), env=test_env)

        self.status["step"] = "package-artifacts"
        package = self.run / "package"
        metadata = package / "metadata"
        metadata.mkdir(parents=True)
        shutil.copy2(str(jar), str(package / filename))
        shutil.copy2(str(source_pom), str(package / ("ame-java-" + version + ".pom")))
        shutil.copy2(str(library), str(package / "libame_jni.so"))
        for name in ("toolchain.txt", "elf-header.txt", "elf-dynamic.txt", "elf-versions.txt", "elf-exports.txt"):
            shutil.copy2(str(native / name), str(metadata / name))
        for name in ("maven-version", "java-version", "javac-version", "NativeLibraryLoaderTest", "AmeEngineSmokeTest", "AmeEngineBoundaryTest"):
            shutil.copy2(str(self.logs / (name + ".log")), str(metadata / (name + ".log")))
        self.status.update(jar_sha256=digest(jar), native_sha256=digest(library),
                           tests={"NativeLibraryLoaderTest": "PASS", "AmeEngineSmokeTest": "PASS", "AmeEngineBoundaryTest": "PASS"})
        write_json(metadata / "build.json", dict(self.status, status="VERIFIED", step="complete"))
        files = sorted(path for path in package.rglob("*") if path.is_file())
        (package / "SHA256SUMS").write_text("".join(
            digest(path) + "  " + path.relative_to(package).as_posix() + "\n" for path in files), encoding="utf-8")
        archive_path = self.run / BUNDLE
        with tarfile.open(str(archive_path), "w:gz") as archive:
            for path in sorted(package.rglob("*")):
                if path.is_file():
                    archive.add(str(path), arcname=path.relative_to(package).as_posix(), recursive=False)
        # Publish only after all build, packaging and JNI checks succeed.
        os.replace(str(archive_path), str(self.artifacts / BUNDLE))
        (self.artifacts / "SHA256SUMS").write_text(digest(self.artifacts / BUNDLE) + "  " + BUNDLE + "\n", encoding="utf-8")
        self.status.update(status="VERIFIED", step="complete")


def main():
    parser = argparse.ArgumentParser(description=__doc__, epilog="Configuration: see .cloudbuild/README.md")
    parser.parse_args()
    require(sys.version_info >= (3, 8), "Python >= 3.8 is required")
    require(platform.system() == "Linux", "Run this entry point in the Linux build environment")
    import fcntl
    require(OUTPUT.resolve() == OUTPUT, "Build output must not use symlink aliases")
    OUTPUT.mkdir(parents=True, exist_ok=True)
    artifacts = OUTPUT / "artifacts"
    runs = OUTPUT / "runs"
    for directory in (artifacts, runs):
        require(directory.resolve() == directory, "Build output must not use symlink aliases")
        directory.mkdir(exist_ok=True)
    lock_path = OUTPUT / ".build.lock"
    require(not lock_path.is_symlink(), "Build lock must not be a symlink")
    with lock_path.open("a") as lock:
        try:
            fcntl.flock(lock.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            raise ValueError("Another build is using this checkout; use a separate CI workspace")
        clear_artifacts(artifacts)
        run = Path(tempfile.mkdtemp(prefix=time.strftime("%Y%m%dT%H%M%SZ-", time.gmtime()), dir=str(runs)))
        build = Build(run, artifacts)
        for sig in (signal.SIGTERM, signal.SIGINT):
            signal.signal(sig, stop)
        try:
            build.build()
        except BaseException:
            clear_artifacts(artifacts)
            raise
        finally:
            write_json(run / "status.json", build.status)
            print("AME CloudBuild logs: " + str(run), flush=True)
        print("AME CloudBuild artifact: " + str(artifacts / BUNDLE), flush=True)


if __name__ == "__main__":
    try:
        main()
    except (ValueError, OSError, subprocess.CalledProcessError) as error:
        print("AME CloudBuild failed: " + str(error), file=sys.stderr)
        sys.exit(error.returncode if isinstance(error, subprocess.CalledProcessError) and error.returncode > 0 else 1)
