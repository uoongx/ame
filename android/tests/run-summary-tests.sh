#!/bin/sh
# Set JSON_JAR to org.json:json:20240303. The test uses the real orchestration with offline fakes.
set -eu
: "${JSON_JAR:?Set JSON_JAR to json-20240303.jar}"
repo=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
src="$repo/android/app/src/main/java/com/ame/memory"
classes=$(mktemp -d "${TMPDIR:-/tmp}/ame-summary-classes.XXXXXX")
trap 'rm -rf "$classes"' EXIT HUP INT TERM
javac --release 17 -encoding UTF-8 -cp "$JSON_JAR" -d "$classes" \
  "$src/MemoryOrchestrator.java" "$src/FollowupGate.java" "$src/MemorySummaryService.java" \
  "$repo/android/tests/summary/Log.java" "$repo/android/tests/summary/SummaryFlowTest.java"
java -cp "$classes:$JSON_JAR" com.ame.memory.SummaryFlowTest
