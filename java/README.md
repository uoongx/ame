# AME Java SDK

`com.huawei.convai:convai-ame-java:<version>`（标准主 JAR，无 classifier）是面向 **Linux x86_64、Java 17+** 的嵌入式记忆 SDK。`<version>` 表示所选 SDK 版本：当前源码版本以 [java/pom.xml](pom.xml) 的顶层 `version` 为准，已出包版本以交付的 `convai-ame-java.pom` 或 `metadata/build.json` 为准。本文介绍当前源码对应版本的实现。

SDK 通过 JNI 调用 AME C++ 引擎，没有第三方 Java 运行依赖，不绑定 Spring、Agent 框架或模型服务。它是应用依赖库，不是可通过 `java -jar` 启动的服务。Java 包已从 `io.ame` 迁移至 `com.huawei.convai.ame`，JNI 资源也迁入对应路径；调用方须更新 import、反射类名或启动类名并重新编译。

4.0 整合分支的 `AmeEngine` 共有 **11 个公共方法**。在 3.0 的 6 个方法基础上，增加了按次租户共享召回、屏蔽、解除屏蔽、删除事件和整库清理。参数、返回值和示例见 [接口参考](docs/API.md)。接入 Agent 时可参阅 [通用集成指南](docs/INTEGRATION.md) 和 [可运行示例](examples/README.md)。

## 构建与交付

在准备好工具链的 Linux x86_64 节点，从仓库根目录执行：

```sh
bash .cloudbuild/build.sh
```

需要 JDK 17+、Maven 3.8+、CMake 3.24+、g++ / make、Python 3.8+ 和 binutils。公司 Maven settings、缓存、并行度及 glibc 上限配置见 [.cloudbuild 接入说明](../.cloudbuild/README.md)。该入口每次在隔离目录重新编译 JNI，打包本次 SO，显式执行真实 JNI 测试；全部成功后输出：

```text
build/cloudbuild/artifacts/convai-ame-java-<version>.jar
build/cloudbuild/artifacts/convai-ame-java.pom
build/cloudbuild/artifacts/convai-ame-java-linux-x86_64.tar.gz
build/cloudbuild/artifacts/SHA256SUMS
```

独立 JAR/POM 可直接作为公司平台制品发布步骤的输入，无需先解包；外部 `SHA256SUMS` 覆盖 JAR、POM 和诊断归档。归档保留与顶层产物一致的 JAR/POM 文件名、`libame_jni.so`、构建与测试元数据及内部 `SHA256SUMS`。交付 POM 固定命名为 `convai-ame-java.pom`，Maven groupId/artifactId 沿用上述坐标，实际版本从 POM 读取，JAR 命名为 `convai-ame-java-<version>.jar`。构建仅打包，不自动执行 Maven `install` 或 `deploy`，也不新增远端发布配置。

归档内 `metadata/build.json` 记录 `group_id: "com.huawei.convai"`、`artifact_id: "convai-ame-java"`、`version: "<version>"`、`java_package: "com.huawei.convai.ame"`、`classifier: null` 和 `platform: "linux-x86_64"`，其中版本字段保存该交付物的实际版本。`java_package` 区分同 GAV 下迁包前后的产物；无 classifier 不表示支持其他操作系统或 CPU 架构。

在源码仓库中解包和校验：

```sh
(cd build/cloudbuild/artifacts && sha256sum -c SHA256SUMS)
mkdir -p build/cloudbuild/sdk
tar -xzf build/cloudbuild/artifacts/convai-ame-java-linux-x86_64.tar.gz -C build/cloudbuild/sdk
(cd build/cloudbuild/sdk && sha256sum -c SHA256SUMS)
```

`java/Dockerfile` 复用 `.cloudbuild` 入口，可从仓库根目录导出同样的流水线产物：

```sh
docker buildx build --platform linux/amd64 -f java/Dockerfile \
  --output type=local,dest=build/docker-artifacts .
```

[Linux 验证记录](docs/LINUX-VALIDATION.md) 保留当时坐标、Java 包和文件名对应的验证证据，不能单凭版本号相同视为迁包后的产物已验证；本次构建以自身元数据和测试日志为准。RC1 验证单独保存在 [历史记录](docs/HISTORICAL-RC1-VALIDATION.md)，原交付产物保持原样。

## 运行条件

当前 SDK 只接受 Linux x86_64 JVM；加载器识别 `amd64/x86_64/x64`。JAR 内唯一 native 资源为 `com/huawei/convai/ame/native/linux-x86_64/libame_jni.so`，JNI ABI 版本为 **2**。初次加载先校验平台，再读取外置路径或提取 JAR 资源；显式 `ame.native.path` 也不能绕过平台检查。

Linux 运行时需满足本次 SO 的实际 glibc 依赖，SDK 不写死 `2.36`。流水线默认仅检测和记录所需版本；只有显式设置 `AME_MAX_GLIBC` 时才执行符号版本上限检查。实际要求见归档 `metadata/build.json` 的 `required_glibc` 和 ELF 记录。面向旧版 Linux 时，应在匹配最低目标环境的工具链/sysroot 中重新构建，并在目标系统验证；修改上限不会降低二进制依赖，静态链接 C++ 运行库也不会消除 glibc 依赖。默认 Docker 示例使用 Debian 12，旧构建的验证数据见 [Linux 验证记录](docs/LINUX-VALIDATION.md)；Alpine/musl 不在此运行范围。

默认加载会将 SO 提取到 `java.io.tmpdir` 下的独立目录，运行环境须允许写临时文件和加载其中的 native library。若临时目录策略不允许加载，可在首次 `open` 前指定匹配的外置 SO：

```sh
java -Dame.native.path=/absolute/path/libame_jni.so -jar your-agent.jar
```

该属性必须指向文件；相对路径会转为绝对路径。错误路径不会静默回退到 JAR 内库。同一类加载器生命周期只加载一次库，之后修改属性不能切换已加载的库。迁包后若仍配置外置 SO，须换用同次构建的 SO；不要混用旧包对应的 JNI 桥或 ABI 不匹配的库。

## 引入依赖

团队制品仓库已有该构件时直接声明依赖即可。仅需在当前机器试用时，可手动将构建输出安装到本地 Maven 仓库；此步骤不是构建入口的默认行为：

```sh
AME_VERSION=$(python3 - <<'PY'
import xml.etree.ElementTree as ET
pom = ET.parse("build/cloudbuild/artifacts/convai-ame-java.pom")
print(pom.findtext("{http://maven.apache.org/POM/4.0.0}version"))
PY
)
JAR="build/cloudbuild/artifacts/convai-ame-java-${AME_VERSION}.jar"
mvn install:install-file \
  -Dfile="$JAR" \
  -DpomFile=build/cloudbuild/artifacts/convai-ame-java.pom
```

上述命令从交付 POM 读取版本，避免使用可能已更新的源码版本。固定名称仅用于交付文件，不改变 Maven 仓库布局。安装后 POM 仍位于仓库根目录下的 `com/huawei/convai/convai-ame-java/<version>/convai-ame-java-<version>.pom`。

在消费工程 `pom.xml` 的 `properties` 中定义 `ame.version` 为所选已发布版本（本地试用时填所安装的版本），再声明依赖。也可在消费工程执行 Maven 时通过 `-Dame.version=所选版本` 传入；该属性由消费工程管理，不自动读取 SDK 源码或升级依赖：

```xml
<dependency>
  <groupId>com.huawei.convai</groupId>
  <artifactId>convai-ame-java</artifactId>
  <version>${ame.version}</version>
</dependency>
```

Gradle 在消费工程的 `gradle.properties` 中设置 `ameVersion` 为所选已发布版本，使用 `implementation "com.huawei.convai:convai-ame-java:${ameVersion}"`，同样不带 classifier，版本由消费工程显式选择。本地安装只影响当前机器；公司平台的制品发布由平台已有步骤管理，本仓库不执行远端发布。历史 `io.ame:ame-java` 构件保持原样。消费时应核对交付版本与 JAR 校验值，避免误用仍含旧 Java 包的本地缓存；源码引用改为以下新包：

```java
import com.huawei.convai.ame.AmeEngine;
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
- `blockEvent/unblockEvent/deleteEvent(eventId)` 修改单条事件，`clearMemory()` 清整个目录对应的记忆。调用 `save()` 或 `close()` 持久化。按次共享召回使用 `recallWithTenant(query, limit, tenant)`。
- `stats()` 返回 `turns/total_chars/events/nodes` JSON；`total_chars` 实际按 UTF-8 字节统计。
- `save()` 全量持久化；`close()` 保存并释放，可重复调用。关闭后其他操作抛 `IllegalStateException`；关闭保存失败仍释放句柄和锁。
- I/O 失败使用 `UncheckedIOException`，无效输入使用 `IllegalArgumentException` 或 `NullPointerException`，平台/native/ABI 加载问题使用 `UnsatisfiedLinkError`。

应用应按可信用户身份选择独立目录并复用实例。`.ame.lock` 拒绝同目录被多个实例或进程同时打开，但不提供租户鉴权或多副本一致性。同一实例的引擎状态由 JNI 实例锁保护；响应式服务应安排到工作线程。上层超时或线程中断不能取消 C++ 操作，实例必须显式关闭。

SDK 不接受模型 key。当前写入使用内置规则提取器和默认向量实现，Java API 没有提取器或 embedding 注入接口。当前上下文、业务规则、模型调用及身份系统继续由应用管理。

## JNI 验证入口

`.cloudbuild` 会显式运行 `AmeEngineSmokeTest` 和 `AmeEngineBoundaryTest`；它们是普通 Java main，`mvn package` 本身不会运行这些检查。以下命令可对构建输出的最终 JAR 重新验证，不设置外置 native 路径：

```sh
AME_VERSION=$(python3 - <<'PY'
import xml.etree.ElementTree as ET
pom = ET.parse("build/cloudbuild/artifacts/convai-ame-java.pom")
print(pom.findtext("{http://maven.apache.org/POM/4.0.0}version"))
PY
)
JAR="build/cloudbuild/artifacts/convai-ame-java-${AME_VERSION}.jar"
CLASSES=java/target/test-classes
mkdir -p "$CLASSES"
javac --release 17 -encoding UTF-8 -cp "$JAR" -d "$CLASSES" \
  java/src/test/java/com/huawei/convai/ame/AmeEngineSmokeTest.java \
  java/src/test/java/com/huawei/convai/ame/AmeEngineBoundaryTest.java
java -cp "$CLASSES:$JAR" com.huawei.convai.ame.AmeEngineSmokeTest
java -Xcheck:jni -cp "$CLASSES:$JAR" com.huawei.convai.ame.AmeEngineBoundaryTest java/target/boundary-tests
```

Smoke 检查中文/emoji 写入召回、Unicode 目录、保存重开、锁、重复关闭和损坏快照保护。Boundary 进一步检查 NUL、无效 UTF-16、JSON Unicode 转义、子 JVM 目录锁及 native 关闭竞争。两者只使用本地合成数据，不调用网络或模型，也不证明真实模型质量、目标存储或生产部署已经通过验收。
