#!/bin/sh
# Pure Java 17 regression; no Android SDK, Gradle, JNI or network is required.
set -eu
repo=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
classes=$(mktemp -d "${TMPDIR:-/tmp}/ame-conversation-classes.XXXXXX")
trap 'rm -rf "$classes"' EXIT HUP INT TERM
javac --release 17 -encoding UTF-8 -d "$classes" \
  "$repo/android/app/src/main/java/com/ame/memory/ConversationWorkQueue.java" \
  "$repo/android/tests/ConversationWorkQueueTest.java"
java -cp "$classes" com.ame.memory.ConversationWorkQueueTest
