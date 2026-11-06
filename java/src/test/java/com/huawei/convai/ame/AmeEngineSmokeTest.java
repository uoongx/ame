package com.huawei.convai.ame;

import java.io.UncheckedIOException;
import java.nio.file.Files;
import java.nio.file.Path;
import java.time.Instant;
import java.util.UUID;
import java.util.logging.Logger;

/** Run explicitly against a real native library; no model/network calls or test dependencies. */
public final class AmeEngineSmokeTest {
  private static final Logger LOG = Logger.getLogger(AmeEngineSmokeTest.class.getName());

  private AmeEngineSmokeTest() {}

  public static void main(String[] args) throws Exception {
    if (args.length > 1) throw new IllegalArgumentException("Usage: AmeEngineSmokeTest [directory]");
    Path base = args.length == 0 ? Files.createTempDirectory("ame-java-smoke-") : Path.of(args[0]);
    // Construct non-ASCII paths inside Java so shell/launcher argument encodings cannot alter them.
    Path directory = base.resolve("中文目录-😀");
    String text = "阿岚喜欢安静的博物馆，也喜欢看青铜器 😊🚀";
    AmeEngine engine = AmeEngine.open(directory);
    try (engine) {
      expectIoFailure(() -> AmeEngine.open(directory));
      String write = engine.rememberTurn("游客😀", text, Instant.now().getEpochSecond(),
          "中文消息-😀", "中文会话-🚀");
      checkJson(write, '{');
      check(write.matches("(?s).*\\\"code\\\"\\s*:\\s*0\\s*[,}].*"), "write must report code 0: " + write);
      String recalled = engine.recall(text, 10);
      checkJson(recalled, '[');
      check(recalled.contains(text), "JNI must preserve Chinese and supplementary Unicode: " + recalled);
      checkJson(engine.stats(), '{');
      engine.save();
    }
    engine.close();
    expectClosed(() -> engine.stats());
    expectClosed(() -> engine.recall(text, 10));
    expectClosed(() -> engine.rememberTurn("游客", text, 1, "", ""));
    expectClosed(engine::save);

    try (AmeEngine reopened = AmeEngine.open(directory)) {
      check(reopened.recall(text, 10).contains(text), "saved Unicode turn must survive reopening");
      checkJson(reopened.stats(), '{');
    }
    verifyForgetting(base.resolve("forget-" + UUID.randomUUID()));
    verifyFailureLifecycle(base.resolve("failure-checks-" + UUID.randomUUID()));
    LOG.info("AME Java/native smoke passed; Unicode snapshot round trip verified");
  }

  private static void verifyForgetting(Path base) {
    Path personal = base.resolve("private");
    Path shared = base.resolve("tenant");
    String id;
    try (AmeEngine memory = AmeEngine.open(personal); AmeEngine tenant = AmeEngine.open(shared)) {
      String write = memory.rememberTurn("user", "private otter", 100, "m1", "s1");
      var match = java.util.regex.Pattern.compile("\"event_id\":\"([^\"]+)\"").matcher(write);
      check(match.find(), "event ID is required");
      id = match.group(1);
      tenant.rememberTurn("guide", "shared walrus", 100, "m2", "s2");
      check(memory.blockEvent(id), "event can be blocked");
      check(!memory.recall("otter", 1).contains("private otter"), "block hides evidence");
      check(memory.recallWithTenant("walrus", 5, tenant).contains("shared walrus"), "shared recall");
      check(!memory.recall("walrus", 5).contains("shared walrus"), "private recall isolation");
    }
    try (AmeEngine memory = AmeEngine.open(personal)) {
      check(!memory.recall("otter", 1).contains("private otter"), "block survives reopen");
      check(memory.unblockEvent(id) && memory.recall("otter", 1).contains("private otter"),
          "unblock restores the event");
      check(memory.deleteEvent(id) && !memory.deleteEvent(id), "delete requires an existing event");
      memory.rememberTurn("user", "a".repeat(23) + "博物馆", 101, "multi", "s2");
      memory.rememberTurn("user", "second otter", 101, "m2", "s2");
    }
    try (AmeEngine memory = AmeEngine.open(personal)) {
      check(!memory.recall("otter", 5).contains("private otter"), "deleted event stays deleted");
      check(memory.recall("otter", 5).contains("second otter"), "other events survive deletion");
      memory.clearMemory();
    }
    try (AmeEngine memory = AmeEngine.open(personal); AmeEngine tenant = AmeEngine.open(shared)) {
      check(memory.recall("otter", 5).equals("[]"), "clear survives reopening");
      check(tenant.recall("walrus", 5).contains("shared walrus"), "private clear preserves tenant");
    }
  }

  private static void verifyFailureLifecycle(Path root) throws Exception {
    Path corrupt = Files.createDirectories(root.resolve("corrupt"));
    String badSnapshot = "{not valid json";
    Files.writeString(corrupt.resolve("graph.json"), badSnapshot);
    expectIoFailure(() -> AmeEngine.open(corrupt));
    check(Files.readString(corrupt.resolve("graph.json")).equals(badSnapshot),
        "opening a damaged snapshot must not overwrite it");

    Path failedSave = root.resolve("failed-save");
    AmeEngine closing = AmeEngine.open(failedSave);
    try (closing) {
      // An empty directory cannot be replaced by the graph.json snapshot file.
      Files.createDirectory(failedSave.resolve("graph.json"));
      expectIoFailure(closing::close);
      closing.close();
      expectClosed(closing::stats);
    }
    Files.delete(failedSave.resolve("graph.json"));
    try (AmeEngine reopened = AmeEngine.open(failedSave)) {
      checkJson(reopened.stats(), '{'); // Failed close must still release the directory lock.
    }
  }

  private static void expectIoFailure(Runnable operation) {
    try {
      operation.run();
      throw new AssertionError("expected an I/O failure");
    } catch (UncheckedIOException expected) {
      check(expected.getCause() != null, "I/O failure must preserve its cause");
    }
  }

  private static void expectClosed(Runnable operation) {
    try {
      operation.run();
      throw new AssertionError("operation on a closed engine must fail");
    } catch (IllegalStateException expected) {
      check(expected.getMessage().contains("closed"), "closed exception must be clear");
    }
  }

  private static void checkJson(String value, char first) {
    check(value != null && !value.isBlank(), "native result must not be empty");
    String trimmed = value.strip();
    check(trimmed.charAt(0) == first, "unexpected JSON shape: " + value);
    check(trimmed.charAt(trimmed.length() - 1) == (first == '{' ? '}' : ']'),
        "unexpected JSON ending: " + value);
  }

  private static void check(boolean condition, String message) {
    if (!condition) throw new AssertionError(message);
  }
}
