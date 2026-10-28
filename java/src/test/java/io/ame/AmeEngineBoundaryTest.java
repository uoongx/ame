package io.ame;

import java.io.UncheckedIOException;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.Executors;
import java.util.concurrent.Future;
import java.util.concurrent.TimeUnit;

/** Native lifecycle and corrupt-snapshot checks; execute against the packaged JNI library. */
public final class AmeEngineBoundaryTest {
  private AmeEngineBoundaryTest() {}

  public static void main(String[] args) throws Exception {
    if (args.length == 2 && args[0].equals("--check-lock")) {
      expectIo(() -> AmeEngine.open(Path.of(args[1])));
      return;
    }
    if (args.length > 1) throw new IllegalArgumentException("Usage: AmeEngineBoundaryTest [directory]");
    Path base = args.length == 0 ? Path.of("target") : Path.of(args[0]);
    Files.createDirectories(base);
    Path root = Files.createDirectory(base.resolve("native-boundary-" + System.nanoTime()));
    unicodeAndDirectoryLock(root);
    corruptedSnapshots(root);
    escapedUnicodeSnapshot(root);
    failedCloseReleasesDirectory(root);
    concurrentNativeClose(root);
    System.out.println("AME native boundary checks passed: " + root.toAbsolutePath());
  }

  private static void unicodeAndDirectoryLock(Path root) throws Exception {
    Path directory = root.resolve("\u8bb0\u5fc6-\ud83d\ude80");
    String text = "\u6587\u7269\ud83d\ude80\u0000\u5386\u53f2";
    try (AmeEngine engine = AmeEngine.open(directory)) {
      expectIo(() -> AmeEngine.open(directory));
      engine.rememberTurn("\u6e38\u5ba2\ud83d\ude00", text, 1700000000L, "m", "s");
      check(engine.recall("\u6587\u7269\ud83d\ude80", 10).contains("\\u0000"), "NUL must survive JSON");
      try {
        engine.rememberTurn("x", "bad\ud800", 0, "", "");
        throw new AssertionError("Unpaired UTF-16 surrogate was accepted");
      } catch (IllegalArgumentException expected) {
        // A native boundary must reject invalid Unicode before mutating the engine.
      }
    }
    try (AmeEngine engine = AmeEngine.open(directory)) {
      check(engine.recall("\u6587\u7269\ud83d\ude80", 10).contains("\ud83d\ude80"), "Unicode directory/text round trip");
    }
    // Exercise Linux child-process arguments and directory locking with a Unicode path.
    Path locked = root.resolve("cross-process-lock-\u8bb0\u5fc6-\ud83d\ude80").toAbsolutePath();
    try (AmeEngine engine = AmeEngine.open(locked)) {
      List<String> command = new ArrayList<>();
      command.add(Path.of(System.getProperty("java.home"), "bin", "java").toString());
      String nativePath = System.getProperty("ame.native.path");
      if (nativePath != null) command.add("-Dame.native.path=" + nativePath);
      command.addAll(List.of("-cp", System.getProperty("java.class.path"),
          AmeEngineBoundaryTest.class.getName(), "--check-lock", locked.toString()));
      Process child = new ProcessBuilder(command).redirectErrorStream(true).start();
      if (!child.waitFor(15, TimeUnit.SECONDS)) {
        child.destroyForcibly();
        throw new AssertionError("Cross-process lock check timed out");
      }
      String output = new String(child.getInputStream().readAllBytes(), StandardCharsets.UTF_8);
      check(child.exitValue() == 0, "A second JVM must not open the same directory: " + output);
      engine.stats();
    }
  }

  private static void corruptedSnapshots(Path root) throws Exception {
    List<String> invalid = List.of(
        "", "not-json", "{}", "[]", "null",
        "{\"graph\":{\"nodes\":[],\"edges\":[{}]}}",
        "{\"graph\":{\"nodes\":[],\"edges\":[]},\"aliases\":{\"bad\":\"\\q\"}}",
        "{\"graph\":{\"nodes\":[],\"edges\":[]},\"aliases\":{\"bad\":\"\\ud800\"}}",
        "{\"graph\":{\"nodes\":[],\"edges\":[]},\"extra\":01}",
        "{\"graph\":{\"nodes\":[],\"edges\":[]},\"extra\":NaN}",
        "[".repeat(200) + "0" + "]".repeat(200));
    for (int i = 0; i < invalid.size(); i++) {
      Path directory = Files.createDirectory(root.resolve("corrupt-" + i));
      Path file = directory.resolve("graph.json");
      byte[] original = invalid.get(i).getBytes(StandardCharsets.UTF_8);
      Files.write(file, original);
      expectIo(() -> AmeEngine.open(directory));
      check(Arrays.equals(Files.readAllBytes(file), original), "Corrupt snapshot must not be overwritten");
    }
  }

  private static void escapedUnicodeSnapshot(Path root) throws Exception {
    Path directory = Files.createDirectory(root.resolve("escaped-unicode"));
    String snapshot = "{\"graph\":{\"nodes\":[{\"uid\":\"test\",\"kind\":0,"
        + "\"text\":\"\\u6587\\u7269\\ud83d\\ude80\",\"ts\":1700000000}],\"edges\":[]},"
        + "\"slots\":{},\"aliases\":{}}";
    Files.writeString(directory.resolve("graph.json"), snapshot);
    try (AmeEngine engine = AmeEngine.open(directory)) {
      check(engine.recall("\u6587\u7269", 10).contains("\u6587\u7269\ud83d\ude80"),
          "JSON UTF-16 escape pairs must be decoded as one Unicode scalar");
    }
  }

  private static void failedCloseReleasesDirectory(Path root) throws Exception {
    Path directory = Files.createDirectory(root.resolve("save-failure"));
    AmeEngine engine = AmeEngine.open(directory);
    Path obstruction = Files.createDirectory(directory.resolve("graph.json"));
    expectIo(engine::save);
    expectIo(engine::close);
    Files.delete(obstruction);
    try (AmeEngine reopened = AmeEngine.open(directory)) {
      reopened.stats();
    }
  }

  private static Method nativeMethod(String name, Class<?>... arguments) throws Exception {
    Method method = AmeEngine.class.getDeclaredMethod(name, arguments);
    method.setAccessible(true);
    return method;
  }

  private static void concurrentNativeClose(Path root) throws Exception {
    // Bypass Java synchronized methods deliberately: the JNI handle registry itself
    // must remain safe if close races operations that already acquired a session.
    Method open = nativeMethod("nativeOpen", String.class);
    Method stats = nativeMethod("nativeStats", long.class);
    Method close = nativeMethod("nativeClose", long.class);
    for (int round = 0; round < 12; round++) {
      long handle = (Long) open.invoke(null, root.resolve("race-" + round).toAbsolutePath().toString());
      var pool = Executors.newFixedThreadPool(5);
      try {
        CountDownLatch start = new CountDownLatch(1);
        List<Future<?>> futures = new ArrayList<>();
        for (int thread = 0; thread < 4; thread++) {
          futures.add(pool.submit(() -> {
            start.await();
            for (int i = 0; i < 100; i++) {
              try {
                stats.invoke(null, handle);
              } catch (InvocationTargetException error) {
                check(error.getCause() instanceof IllegalStateException, "Unexpected native race failure");
              }
            }
            return null;
          }));
        }
        futures.add(pool.submit(() -> { start.await(); close.invoke(null, handle); return null; }));
        start.countDown();
        for (Future<?> future : futures) future.get(15, TimeUnit.SECONDS);
      } finally {
        pool.shutdownNow();
        close.invoke(null, handle);
      }
    }
  }

  private static void expectIo(Runnable operation) {
    try {
      operation.run();
      throw new AssertionError("Expected an I/O failure");
    } catch (UncheckedIOException expected) {
      // Expected boundary failure; the caller must retain the original snapshot.
    }
  }

  private static void check(boolean condition, String message) {
    if (!condition) throw new AssertionError(message);
  }
}
