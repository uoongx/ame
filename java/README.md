# AME Java SDK

`io.ame:ame-java:3.0.0-rc.2`（classifier：`linux-x86_64`）是面向 **Linux x86_64、Java 17+** 的嵌入式记忆 SDK。它通过 JNI 调用 AME C++ 引擎，没有第三方 Java 运行依赖，不绑定 Spring、Agent 框架或模型服务。SDK 是应用依赖库，不是可通过 `java -jar` 启动的服务。

接入其他 Agent 时，使用 [接口参考](docs/API.md)、[通用集成指南](docs/INTEGRATION.md) 和 [可运行示例](examples/README.md)。provider、回退、身份鉴权与调度由应用实现，SDK 提供同步的记忆读写接口。

## 构建与交付

在准备好工具链的 Linux x86_64 节点，从仓库根目录执行：

```sh
sh .cloudbuild/build.sh
```

需要 JDK 17+、Maven 3.8+、CMake 3.24+、g++ / make、Python 3.8+ 和 binutils。公司 Maven settings、缓存、并行度及 glibc 上限配置见 [.cloudbuild 接入说明](../.cloudbuild/README.md)。该入口每次在隔离目录重新编译 JNI，打包本次 SO，显式执行真实 JNI 测试；全部成功后输出：

```text
build/cloudbuild/artifacts/ame-java-linux-x86_64.tar.gz
build/cloudbuild/artifacts/SHA256SUMS
```

归档中的主要文件是 `ame-java-3.0.0-rc.2-linux-x86_64.jar`、配套 `ame-java-3.0.0-rc.2.pom`、`libame_jni.so`、构建与测试元数据、`SHA256SUMS`。版本取自 POM，固定归档名便于流水线收集。构建不会自动安装或发布到 Maven 仓库。

在源码仓库中解包和校验：

```sh
(cd build/cloudbuild/artifacts && sha256sum -c SHA256SUMS)
mkdir -p build/cloudbuild/sdk
tar -xzf build/cloudbuild/artifacts/ame-java-linux-x86_64.tar.gz -C build/cloudbuild/sdk
(cd build/cloudbuild/sdk && sha256sum -c SHA256SUMS)
```

`java/Dockerfile` 复用 `.cloudbuild` 入口，可从仓库根目录导出同样的流水线产物：

```sh
docker buildx build --platform linux/amd64 -f java/Dockerfile \
  --output type=local,dest=build/docker-artifacts .
```

脚本和 Dockerfile 的实际验证范围见 [Linux 验证记录](docs/LINUX-VALIDATION.md)。旧版本验证单独保存在 [RC1 历史记录](docs/HISTORICAL-RC1-VALIDATION.md)，不能用于证明 RC2 已通过验证。原交付产物保持原样。

## 运行条件

当前 SDK 只接受 Linux x86_64 JVM；加载器识别 `amd64/x86_64/x64`。JAR 内唯一 native 资源为 `io/ame/native/linux-x86_64/libame_jni.so`，JNI ABI 版本为 **1**。初次加载先校验平台，再读取外置路径或提取 JAR 资源；显式 `ame.native.path` 也不能绕过平台检查。

Linux 运行时需满足本次 SO 的实际 glibc 依赖，SDK 不写死 `2.36`。流水线默认仅检测和记录所需版本；只有显式设置 `AME_MAX_GLIBC` 时才执行符号版本上限检查。实际要求见归档 `metadata/build.json` 的 `required_glibc` 和 ELF 记录。面向旧版 Linux 时，应在匹配最低目标环境的工具链/sysroot 中重新构建，并在目标系统验证；修改上限不会降低二进制依赖，静态链接 C++ 运行库也不会消除 glibc 依赖。默认 Docker 示例使用 Debian 12，旧构建的验证数据见 [Linux 验证记录](docs/LINUX-VALIDATION.md)；Alpine/musl 不在此运行范围。

默认加载会将 SO 提取到 `java.io.tmpdir` 下的独立目录，运行环境须允许写临时文件和加载其中的 native library。若临时目录策略不允许加载，可在首次 `open` 前指定匹配的外置 SO：

```sh
java -Dame.native.path=/absolute/path/libame_jni.so -jar your-agent.jar
```

该属性必须指向文件；相对路径会转为绝对路径。错误路径不会静默回退到 JAR 内库。同一类加载器生命周期只加载一次库，之后修改属性不能切换已加载的库。不要混用其他 JNI 桥或 ABI 不匹配的库。

## 引入依赖

收到归档后，先按上述步骤解包，再安装配套 JAR/POM：

```sh
mvn install:install-file \
  -Dfile=build/cloudbuild/sdk/ame-java-3.0.0-rc.2-linux-x86_64.jar \
  -DpomFile=build/cloudbuild/sdk/ame-java-3.0.0-rc.2.pom \
  -Dclassifier=linux-x86_64
```

应用声明：

```xml
<dependency>
  <groupId>io.ame</groupId>
  <artifactId>ame-java</artifactId>
  <version>3.0.0-rc.2</version>
  <classifier>linux-x86_64</classifier>
</dependency>
```

本地安装不等于团队发布。团队 Maven 发布也必须保留 classifier；不要将此产物改名后覆盖旧的无 classifier 坐标。

```java
import io.ame.AmeEngine;
import java.nio.file.Path;
import java.time.Instant;

try (AmeEngine memory = AmeEngine.open(Path.of("data", "visitor-123"))) {
  String written = memory.rememberTurn("游客", "我喜欢安静的博物馆 😊",
      Instant.now().getEpochSecond(), "message-42", "session-7");
  String evidence = memory.recall("喜欢什么样的博物馆", 5);
  String stats = memory.stats();
  memory.save();
  // 应用用自己的 JSON 库解析 evidence，再交给现有模型生成回答。
}
```

- `open(Path)` 创建目录或读取快照；坏快照会失败，不会自动清空。
- `rememberTurn(...)` 返回写入 JSON，例如 `{"code":0,"event_id":"...","resolved_entities":[]}`。String 参数不可为 null；messageId/sessionId 可为空，均为来源标记，不提供幂等或隔离。
- `recall(query, limit)` 返回包含 `event_id/text/ts/score/explain` 的 JSON 数组；limit 为 1–100，无结果为 `[]`。它返回证据，不生成答案。
- `stats()` 返回 `turns/total_chars/events/nodes` JSON；`total_chars` 实际按 UTF-8 字节统计。
- `save()` 全量持久化；`close()` 保存并释放，可重复调用。关闭后其他操作抛 `IllegalStateException`；关闭保存失败仍释放句柄和锁。
- I/O 失败使用 `UncheckedIOException`，无效输入使用 `IllegalArgumentException` 或 `NullPointerException`，平台/native/ABI 加载问题使用 `UnsatisfiedLinkError`。

应用应按可信用户身份选择独立目录并复用实例。`.ame.lock` 拒绝同目录被多个实例或进程同时打开，但不提供租户鉴权或多副本一致性。每个实例的方法通过 `synchronized` 串行执行；响应式服务应安排到工作线程。上层超时或线程中断不能取消 C++ 操作，实例必须显式关闭。

SDK 不接受模型 key。当前写入使用内置规则提取器和默认向量实现，Java API 没有提取器或 embedding 注入接口。当前上下文、业务规则、模型调用及身份系统继续由应用管理。

## JNI 验证入口

`.cloudbuild` 会显式运行 `AmeEngineSmokeTest` 和 `AmeEngineBoundaryTest`；它们是普通 Java main，`mvn package` 本身不会运行这些检查。以下命令可对解包后的最终 JAR 重新验证，不设置外置 native 路径：

```sh
JAR=build/cloudbuild/sdk/ame-java-3.0.0-rc.2-linux-x86_64.jar
CLASSES=java/target/test-classes
mkdir -p "$CLASSES"
javac --release 17 -encoding UTF-8 -cp "$JAR" -d "$CLASSES" \
  java/src/test/java/io/ame/AmeEngineSmokeTest.java \
  java/src/test/java/io/ame/AmeEngineBoundaryTest.java
java -cp "$CLASSES:$JAR" io.ame.AmeEngineSmokeTest
java -Xcheck:jni -cp "$CLASSES:$JAR" io.ame.AmeEngineBoundaryTest java/target/boundary-tests
```

Smoke 检查中文/emoji 写入召回、Unicode 目录、保存重开、锁、重复关闭和损坏快照保护。Boundary 进一步检查 NUL、无效 UTF-16、JSON Unicode 转义、子 JVM 目录锁及 native 关闭竞争。两者只使用本地合成数据，不调用网络或模型，也不证明真实模型质量、目标存储或生产部署已经通过验收。
