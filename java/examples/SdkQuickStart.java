import io.ame.AmeEngine;
import java.io.IOException;
import java.nio.file.Files;
import java.nio.file.Path;
import java.time.Instant;
import java.util.UUID;

/** Java 17+, AME JAR only. Uses synthetic data; requires a matching native library. */
public final class SdkQuickStart {
  public static void main(String[] args) throws IOException {
    if (args.length > 1) {
      throw new IllegalArgumentException("Usage: SdkQuickStart [new-or-existing-demo-directory]");
    }
    Path directory = args.length == 0
        ? Files.createTempDirectory("ame-quickstart-")
        : Path.of(args[0]).toAbsolutePath().normalize();
    System.out.println("演示数据目录：" + directory);
    String messagePrefix = UUID.randomUUID().toString();
    long now = Instant.now().getEpochSecond(); // SDK timestamps are epoch SECONDS.

    try (AmeEngine memory = AmeEngine.open(directory)) {
      System.out.println("写入游客消息：" + memory.rememberTurn(
          "游客", "我喜欢安静的博物馆，尤其喜欢青铜器。😊", now,
          messagePrefix + ":user", "demo-session"));
      System.out.println("写入助手消息：" + memory.rememberTurn(
          "助手", "可以选择人少的时段，慢慢欣赏青铜器展览。", now,
          messagePrefix + ":assistant", "demo-session"));
      System.out.println("召回 JSON：" + memory.recall("喜欢安静的博物馆和青铜器", 3));
      System.out.println("保存前统计：" + memory.stats());
      memory.save(); // Explicit checkpoint; close() also saves and releases the directory lock.
    }

    // Reopen only after the previous instance has closed. JSON is printed intact, not parsed here.
    try (AmeEngine reopened = AmeEngine.open(directory)) {
      System.out.println("重开后召回 JSON：" + reopened.recall("喜欢安静的博物馆和青铜器", 3));
      System.out.println("重开后统计：" + reopened.stats());
    }
    // The directory is deliberately retained so the saved snapshot can be inspected.
  }
}
