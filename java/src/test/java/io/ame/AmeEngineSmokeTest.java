package io.ame;

import java.io.UncheckedIOException;
import java.nio.file.Files;
import java.nio.file.Path;
import java.time.Instant;
import java.util.UUID;

/** Run explicitly against a real native library; no model/network calls or test dependencies. */
public final class AmeEngineSmokeTest {
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
    verifyFailureLifecycle(base.resolve("failure-checks-" + UUID.randomUUID()));
    System.out.println("AME Java/native smoke passed; snapshot directory: " + directory.toAbsolutePath());
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
