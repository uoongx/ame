package io.ame.examples;

import io.ame.AmeEngine;
import java.io.IOException;
import java.nio.file.Files;
import java.nio.file.Path;
import java.time.Instant;
import java.util.UUID;
import java.util.logging.Level;
import java.util.logging.Logger;

/** Java 17+, AME JAR only. Uses synthetic data; requires a matching native library. */
public final class SdkQuickStart {
  private static final Logger LOG = Logger.getLogger(SdkQuickStart.class.getName());

  public static void main(String[] args) throws IOException {
    if (args.length > 1) {
      throw new IllegalArgumentException("Usage: io.ame.examples.SdkQuickStart [new-or-existing-demo-directory]");
    }
    Path directory = args.length == 0
        ? Files.createTempDirectory("ame-quickstart-")
        : Path.of(args[0]).toAbsolutePath().normalize();
    LOG.log(Level.INFO, "演示数据目录：{0}", directory);
    String messagePrefix = UUID.randomUUID().toString();
    long now = Instant.now().getEpochSecond(); // SDK timestamps are epoch SECONDS.

    try (AmeEngine memory = AmeEngine.open(directory)) {
      String userWrite = memory.rememberTurn(
          "游客", "我喜欢安静的博物馆，尤其喜欢青铜器。😊", now,
          messagePrefix + ":user", "demo-session");
      LOG.log(Level.INFO, "写入游客消息：{0}", userWrite);
      String assistantWrite = memory.rememberTurn(
          "助手", "可以选择人少的时段，慢慢欣赏青铜器展览。", now,
          messagePrefix + ":assistant", "demo-session");
      LOG.log(Level.INFO, "写入助手消息：{0}", assistantWrite);
      String recalled = memory.recall("喜欢安静的博物馆和青铜器", 3);
      LOG.log(Level.INFO, "召回 JSON：{0}", recalled);
      String stats = memory.stats();
      LOG.log(Level.INFO, "保存前统计：{0}", stats);
      memory.save(); // Explicit checkpoint; close() also saves and releases the directory lock.
    }

    // Reopen only after the previous instance has closed. JSON is logged intact, not parsed here.
    try (AmeEngine reopened = AmeEngine.open(directory)) {
      String recalled = reopened.recall("喜欢安静的博物馆和青铜器", 3);
      LOG.log(Level.INFO, "重开后召回 JSON：{0}", recalled);
      String stats = reopened.stats();
      LOG.log(Level.INFO, "重开后统计：{0}", stats);
    }
    // The directory is deliberately retained so the saved snapshot can be inspected.
  }
}
