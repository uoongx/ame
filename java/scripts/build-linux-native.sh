#!/bin/sh
# Run inside a Linux builder with CMake, a GNU-compatible C++ compiler, JDK 17+, binutils.
# Usage: sh java/scripts/build-linux-native.sh /absolute/output-directory
set -eu
AME_SOURCE_ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
AME_OUTPUT=${1:?Supply a dedicated absolute output directory}
case "$AME_OUTPUT" in /*) ;; *) printf '%s\n' 'Output directory must be absolute' >&2; exit 2 ;; esac
test "$(uname -s)" = Linux
JAVA_HOME=$(dirname "$(dirname "$(readlink -f "$(command -v javac)")")")
export JAVA_HOME
mkdir -p "$AME_OUTPUT"
{
  date -u
  cat /etc/os-release
  uname -m
  g++ --version
  cmake --version
  java -version 2>&1
} > "$AME_OUTPUT/toolchain.txt"
cmake -S "$AME_SOURCE_ROOT/java/native" -B "$AME_OUTPUT/native-build" \
  -DCMAKE_BUILD_TYPE=Release \
  '-DCMAKE_SHARED_LINKER_FLAGS=-static-libstdc++ -static-libgcc -Wl,--exclude-libs,ALL'
AME_BUILD_JOBS=${AME_BUILD_JOBS:-2}
case "$AME_BUILD_JOBS" in ''|0|*[!0-9]*) printf '%s\n' 'AME_BUILD_JOBS must be a positive integer' >&2; exit 2 ;; esac
cmake --build "$AME_OUTPUT/native-build" --parallel "$AME_BUILD_JOBS"
AME_LIBRARY="$AME_OUTPUT/native-build/libame_jni.so"
readelf -h "$AME_LIBRARY" > "$AME_OUTPUT/elf-header.txt"
readelf -d "$AME_LIBRARY" > "$AME_OUTPUT/elf-dynamic.txt"
readelf --version-info "$AME_LIBRARY" > "$AME_OUTPUT/elf-versions.txt"
nm -D --defined-only "$AME_LIBRARY" > "$AME_OUTPUT/elf-exports.txt"
sha256sum "$AME_LIBRARY" > "$AME_OUTPUT/native.sha256"
for AME_SYMBOL in JNI_OnLoad nativeOpen nativeRememberTurn nativeRecall nativeStats nativeSave nativeClose nativeVersion; do
  case "$AME_SYMBOL" in JNI_OnLoad) ;; *) AME_SYMBOL="Java_com_huawei_convai_ame_AmeEngine_$AME_SYMBOL" ;; esac
  grep -q " $AME_SYMBOL$" "$AME_OUTPUT/elf-exports.txt" || { printf 'Missing export: %s\n' "$AME_SYMBOL" >&2; exit 1; }
done
if grep 'NEEDED' "$AME_OUTPUT/elf-dynamic.txt" | grep -E 'libstdc\+\+|libgcc_s|libjvm'; then
  printf '%s\n' 'Unexpected external C++ or JVM runtime dependency' >&2
  exit 1
fi
cat "$AME_OUTPUT/native.sha256"
grep 'NEEDED' "$AME_OUTPUT/elf-dynamic.txt"
printf '%s\n' 'Linux native build and ELF checks passed; JVM tests must run against the packaged JAR.'
