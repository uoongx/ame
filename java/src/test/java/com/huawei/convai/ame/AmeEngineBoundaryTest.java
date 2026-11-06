package com.huawei.convai.ame;

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
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.Future;
import java.util.concurrent.TimeUnit;
import java.util.logging.Logger;

/** Native lifecycle and corrupt-snapshot checks; execute against the packaged JNI library. */
public final class AmeEngineBoundaryTest {
  private static final Logger LOG = Logger.getLogger(AmeEngineBoundaryTest.class.getName());

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
    invalidSnapshotFields(root);
    jsonGrammarSnapshots(root);
    malformedUtf8Snapshots(root);
    escapedUnicodeSnapshot(root);
    unicodeScalarBoundaries(root);
    failedCloseReleasesDirectory(root);
    concurrentNativeClose(root);
    concurrentTenantClose(root);
    LOG.info("AME native boundary checks passed: " + root.toAbsolutePath());
  }

  private static void unicodeAndDirectoryLock(Path root) throws Exception {
    Path directory = root.resolve("\u8bb0\u5fc6-\ud83d\ude80");
    String text = "\u6587\u7269\ud83d\ude80\u0000\u5386\u53f2";
    try (AmeEngine engine = AmeEngine.open(directory)) {
      expectIo(() -> AmeEngine.open(directory));
      engine.rememberTurn("\u6e38\u5ba2\ud83d\ude00", text, 1700000000L, "m", "s");
      check(engine.recall("\u6587\u7269\ud83d\ude80", 10).contains("\\u0000"), "NUL must survive JSON");
      String before = engine.stats();
      for (String invalid : List.of("bad\ud800", "\udc00", "\ud800x", "\ud800\ud800")) {
        try {
          engine.rememberTurn("x", invalid, 0, "", "");
          throw new AssertionError("Unpaired UTF-16 surrogate was accepted");
        } catch (IllegalArgumentException expected) {
          check(before.equals(engine.stats()), "Invalid Unicode must not mutate the engine");
        }
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
      rejectSnapshot(root.resolve("corrupt-" + i), invalid.get(i).getBytes(StandardCharsets.UTF_8));
    }
  }

  private static void rejectSnapshot(Path directory, byte[] original) throws Exception {
    Files.createDirectory(directory);
    Path file = directory.resolve("graph.json");
    Files.write(file, original);
    expectIo(() -> AmeEngine.open(directory));
    check(Arrays.equals(Files.readAllBytes(file), original), "Corrupt snapshot must not be overwritten");
  }

  private static void invalidSnapshotFields(Path root) throws Exception {
    String node = "{\"uid\":\"test\",\"kind\":0}";
    String graph = "{\"nodes\":[" + node + "],\"edges\":[]}";
    List<String> invalid = List.of(
        "{\"nodes\":[" + node + "," + node + "],\"edges\":[]}",
        "{\"nodes\":[{\"uid\":\"test\",\"kind\":8}],\"edges\":[]}",
        "{\"nodes\":[{\"uid\":\"test\",\"kind\":0,\"ts\":1.5}],\"edges\":[]}",
        "{\"nodes\":[{\"uid\":\"test\",\"kind\":0,\"embedding\":[1e100]}],\"edges\":[]}",
        "{\"nodes\":[" + node + "],\"edges\":[{\"from\":\"test\",\"to\":\"missing\",\"type\":\"BAD\"}]}",
        "{\"nodes\":[" + node + "],\"edges\":[{\"from\":\"test\",\"to\":\"test\",\"type\":\"BAD\"}]}",
        "{\"graph\":" + graph + ",\"aliases\":{\"bad\":1}}",
        "{\"graph\":" + graph + ",\"slots\":{\"missing\":[]}}",
        "{\"graph\":" + graph + ",\"slots\":{\"test\":[{\"slot\":\"x\",\"value\":1}]}}",
        "{\"graph\":" + graph + ",\"slots\":{\"test\":[{\"slot\":\"x\",\"value\":\"y\",\"valid_from\":1e20}]}}");
    for (int i = 0; i < invalid.size(); i++) {
      rejectSnapshot(root.resolve("fields-" + i), invalid.get(i).getBytes(StandardCharsets.UTF_8));
    }
  }

  private static void jsonGrammarSnapshots(Path root) throws Exception {
    String prefix = "{\"nodes\":[],\"edges\":[],\"extra\":";
    List<String> invalidValues = List.of(
        "-", "+1", ".1", "1.", "1e", "1e+", "1e-", "1e9999", "0x1", "1 2", "--1", "-01",
        "trueX", "fals", "nullx", "[1,]", "{\"a\":1,}", "{\"a\":1,\"a\":2}",
        "\"\\u12x4\"", "\"\\udc00\"", "\"\\ud800\\u0041\"", "\"\\ud800x\"", "\"\u0001\"");
    for (int i = 0; i < invalidValues.size(); i++) {
      rejectSnapshot(root.resolve("grammar-" + i),
          (prefix + invalidValues.get(i) + "}").getBytes(StandardCharsets.UTF_8));
    }
    // Exercise every literal/escape and valid number shape alongside the original bare graph format.
    String valid = prefix + "[true,false,null,{},[],0,-0,-12,1.25,1e+2,-2E-2,"
        + "\"\\\"\\\\\\/\\b\\f\\n\\r\\t\\u0000\"]}";
    Path directory = Files.createDirectory(root.resolve("grammar-valid"));
    Files.writeString(directory.resolve("graph.json"), valid);
    try (AmeEngine engine = AmeEngine.open(directory)) {
      engine.stats();
    }
  }

  private static void malformedUtf8Snapshots(Path root) throws Exception {
    List<byte[]> invalid = List.of(
        new byte[] {(byte) 0x80},
        new byte[] {(byte) 0xc0, (byte) 0xaf},
        new byte[] {(byte) 0xc2, 0x20},
        new byte[] {(byte) 0xe0, (byte) 0x80, (byte) 0x80},
        new byte[] {(byte) 0xed, (byte) 0xa0, (byte) 0x80},
        new byte[] {(byte) 0xf0, (byte) 0x80, (byte) 0x80, (byte) 0x80},
        new byte[] {(byte) 0xf4, (byte) 0x90, (byte) 0x80, (byte) 0x80},
        new byte[] {(byte) 0xf5, (byte) 0x80, (byte) 0x80, (byte) 0x80},
        new byte[] {(byte) 0xff},
        new byte[] {(byte) 0xf0, (byte) 0x90});
    byte[] prefix = "{\"nodes\":[],\"edges\":[],\"extra\":\"".getBytes(StandardCharsets.UTF_8);
    for (int i = 0; i < invalid.size(); i++) {
      byte[] sequence = invalid.get(i);
      byte[] snapshot = Arrays.copyOf(prefix, prefix.length + sequence.length + 2);
      System.arraycopy(sequence, 0, snapshot, prefix.length, sequence.length);
      snapshot[snapshot.length - 2] = '"';
      snapshot[snapshot.length - 1] = '}';
      rejectSnapshot(root.resolve("utf8-" + i), snapshot);
    }
    byte[] truncated = Arrays.copyOf(prefix, prefix.length + 1);
    truncated[truncated.length - 1] = (byte) 0xf0;
    rejectSnapshot(root.resolve("utf8-truncated"), truncated);
  }

  private static void unicodeScalarBoundaries(Path root) throws Exception {
    String text = "Unicode:\u007f\u0080\u07ff\u0800\ud7ff\ue000\uffff\ud800\udc00\udbff\udfff";
    Path directory = root.resolve("unicode-boundaries");
    try (AmeEngine engine = AmeEngine.open(directory)) {
      engine.rememberTurn("user", text, 1700000000L, "m", "s");
      check(engine.recall("Unicode", 10).contains(text), "UTF-16 to UTF-8 scalar boundaries");
    }
    try (AmeEngine engine = AmeEngine.open(directory)) {
      check(engine.recall("Unicode", 10).contains(text), "Snapshot UTF-8 scalar boundaries");
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

  private static void concurrentTenantClose(Path root) throws Exception {
    try (AmeEngine session = AmeEngine.open(root.resolve("private"));
         AmeEngine tenant = AmeEngine.open(root.resolve("shared"))) {
      tenant.rememberTurn("guide", "tenant walrus", 1, "", "");
      var start = new CountDownLatch(1);
      var active = new CountDownLatch(2);
      var pool = Executors.newFixedThreadPool(3);
      try {
        var forward = pool.submit(() -> tenantReads(start, active, session, tenant));
        var reverse = pool.submit(() -> tenantReads(start, active, tenant, session));
        start.countDown();
        check(active.await(10, TimeUnit.SECONDS), "both directions must complete a shared query");
        tenant.close();
        forward.get(10, TimeUnit.SECONDS);
        reverse.get(10, TimeUnit.SECONDS);
        check(session.recall("walrus", 1).equals("[]"), "closed tenant must stay isolated");
      } finally {
        pool.shutdownNow();
      }
    }
  }

  private static Void tenantReads(CountDownLatch start, CountDownLatch active,
                                  AmeEngine reader, AmeEngine other)
      throws Exception {
    start.await();
    check(reader.recallWithTenant("walrus", 1, other).contains("tenant walrus"), "shared read");
    active.countDown();
    for (int i = 0; i < 50; ++i) {
      try {
        reader.recallWithTenant("walrus", 1, other);
      } catch (IllegalStateException closed) {
        check(closed.getMessage().contains("closed"), "expected a closed tenant handle");
        break;
      }
    }
    return null;
  }

  private static void concurrentNativeClose(Path root) throws Exception {
    // Bypass Java synchronized methods deliberately: the JNI handle registry itself
    // must remain safe if close races operations that already acquired a session.
    Method open = nativeMethod("nativeOpen", String.class);
    Method stats = nativeMethod("nativeStats", long.class);
    Method close = nativeMethod("nativeClose", long.class);
    for (int round = 0; round < 12; round++) {
      runNativeCloseRace(root.resolve("race-" + round), open, stats, close);
    }
  }

  private static void runNativeCloseRace(Path directory, Method open, Method stats, Method close)
      throws Exception {
    long handle = (Long) open.invoke(null, directory.toAbsolutePath().toString());
    var pool = Executors.newFixedThreadPool(5);
    try {
      CountDownLatch start = new CountDownLatch(1);
      List<Future<?>> futures = submitStatsReaders(pool, start, stats, handle);
      futures.add(pool.submit(() -> { start.await(); close.invoke(null, handle); return null; }));
      start.countDown();
      for (Future<?> future : futures) future.get(15, TimeUnit.SECONDS);
    } finally {
      pool.shutdownNow();
      close.invoke(null, handle);
    }
  }

  private static List<Future<?>> submitStatsReaders(ExecutorService pool, CountDownLatch start,
                                                   Method stats, long handle) {
    List<Future<?>> futures = new ArrayList<>();
    for (int thread = 0; thread < 4; thread++) {
      futures.add(pool.submit(() -> readStatsAfterStart(start, stats, handle)));
    }
    return futures;
  }

  private static Void readStatsAfterStart(CountDownLatch start, Method stats, long handle)
      throws Exception {
    start.await();
    for (int i = 0; i < 100; i++) {
      readStatsAllowingClosedHandle(stats, handle);
    }
    return null;
  }

  private static void readStatsAllowingClosedHandle(Method stats, long handle) throws Exception {
    try {
      stats.invoke(null, handle);
    } catch (InvocationTargetException error) {
      check(error.getCause() instanceof IllegalStateException, "Unexpected native race failure");
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
