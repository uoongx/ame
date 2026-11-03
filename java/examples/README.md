# 不依赖框架的 Java 示例

这两个示例面向 Linux x86_64，只依赖 Java 17+ 和 `convai-ame-java-<version>.jar`（`com.huawei.convai:convai-ame-java:<version>`，无 classifier），无需 Spring、JSON 库或模型服务。`<version>` 表示所选 SDK 版本：当前源码版本以 [java/pom.xml](../pom.xml) 的顶层 `version` 为准，已出包版本以交付的 `convai-ame-java.pom` 或 `metadata/build.json` 为准。示例描述当前源码对应版本的用法。

SDK Java 包已迁移至 `com.huawei.convai.ame`，JNI 资源为 `com/huawei/convai/ame/native/linux-x86_64/libame_jni.so`；调用方须更新 import 并重新编译，Linux 平台限制不变。示例全部使用合成中文数据，请使用专用演示目录。

- `SdkQuickStart.java`：写入两条消息，召回、查看统计、保存，然后关闭并重新打开同一目录。
- `PluggableAgentExample.java`：用应用自己的 `ContextProvider` 选择 legacy 或 AME。默认 legacy；模型回调记录输入消息的角色和长度并返回固定文本，没有调用真实 LLM，也不会解释召回 JSON。

## 编译与运行

以下命令从 AME 仓库根目录执行，从交付 POM 读取实际版本并选择同次构建的独立 JAR。构建仅打包，不自动安装或远端发布；收到现成产物时可跳过构建。若只收到诊断归档，可按 [SDK README](../README.md) 解包，将下面读取 POM 的路径和 `JAR` 路径一并改为解包目录。编译需要 JDK 17+，`--release 17` 保持 Java 17 字节码兼容。两个示例已迁入 `com.huawei.convai.ame.examples` 包；源文件保留在当前目录，`javac -d` 会生成对应的包目录，运行时使用下面的新完整类名。

自动读取 POM 的辅助命令需要 Python 3；没有 Python 时，可省略 `AME_VERSION` 读取段，直接把 `JAR` 设为交付文件的实际路径。SDK 运行本身不依赖 Python。

```sh
sh .cloudbuild/build.sh
(cd build/cloudbuild/artifacts && sha256sum -c SHA256SUMS)
AME_VERSION=$(python3 - <<'PY'
import xml.etree.ElementTree as ET
pom = ET.parse("build/cloudbuild/artifacts/convai-ame-java.pom")
print(pom.findtext("{http://maven.apache.org/POM/4.0.0}version"))
PY
)
JAR="build/cloudbuild/artifacts/convai-ame-java-${AME_VERSION}.jar"
OUT=java/target/example-classes
mkdir -p "$OUT"
javac -encoding UTF-8 --release 17 -cp "$JAR" -d "$OUT" \
  java/examples/SdkQuickStart.java java/examples/PluggableAgentExample.java

# 默认 legacy：不打开 AME，也不加载 native library。
java -cp "$OUT:$JAR" com.huawei.convai.ame.examples.PluggableAgentExample

# SDK 闭环：创建并保留临时演示目录；也可在类名后传入专用目录。
java -cp "$OUT:$JAR" com.huawei.convai.ame.examples.SdkQuickStart

# AME：同一个可信身份的实例在两次请求之间复用。
java -Dexample.context=ame -Dexample.dataRoot=java/target/example-memory \
  -cp "$OUT:$JAR" com.huawei.convai.ame.examples.PluggableAgentExample
```

QuickStart 和 AME 模式需要 Linux x86_64 JVM 与兼容的 glibc。JAR 内置对应 SO，通常无需配置 native 路径；如需外置库，可在 `java` 后、主类名前加 `-Dame.native.path=/absolute/path/libame_jni.so`。加载器在处理外置路径前仍校验平台。构建与运行条件见 [SDK README](../README.md)。

native 无法加载时 QuickStart 明确失败；可插拔示例记录 WARNING 级别的 `[open]` 日志并回退 legacy。后者退出成功不代表 AME 可用，应检查告警和第二次模型输入中的历史附加消息。实际 SO 的 glibc 要求与本版本验证范围见 [Linux 验证记录](../docs/LINUX-VALIDATION.md)。

## 应用与 SDK 的边界

`example.context`、`example.dataRoot`、`ContextProvider`、`Agent` 和 `Model` 都是此示例的应用代码，**不是 SDK 配置或 SDK 公共接口**。SDK 的 native 路径配置是 `ame.native.path`。示例未设置模型 URL、API key、embedding 或提取器参数。

示例把服务端已验证身份表示为固定的合成 namespace，SHA-256 后作为数据目录名。生产代码应从自身认证、授权系统取得这个值；不要信任请求体里的用户 ID。哈希只生成安全的目录名，不代替身份校验或目录访问权限。一个 engine 对应一个身份作用域；`sessionId` 仅是来源标记，不负责隔离。不要将本例共享的合成身份复制到真实多用户入口。

读失败时保留原始 prompt，本轮跳过回写；写或保存失败时告警并保留已经成功的回答。模型调用位于两个记忆异常处理块之外，每请求仅调用一次；模型抛异常、返回空内容或未完成时不会回写。流式应用需要在确认流成功结束后，传入完整回答，排除 reasoning 与部分输出。模型失败或用户断连的识别由应用实现。

示例只捕获预期的 SDK `UncheckedIOException`、`IllegalStateException`，以及初始化时明确的 `UnsatisfiedLinkError`；没有捕获通用 `Exception`、`Error` 或 `Throwable` 来隐藏程序错误。模型抛出的异常继续传播。两条追加和一次 `save()` 不是事务；失败后可能部分写入，因此没有自动重试或幂等承诺。

示例使用 JDK 自带的 `java.util.logging`，正常过程为 INFO、降级告警为 WARNING，可通过 `-Djava.util.logging.config.file=/path/to/logging.properties` 调整日志输出。固定日志文案为英文，只记录模式、目录、ASCII 角色、调用次数与文本/JSON 长度，不打印提示词、回答或召回 JSON 的内容；`chars` 和 `jsonChars` 均为 Java 字符串的 UTF-16 代码单元数。中文合成数据仍参与模型回调、记忆读写和持久化。异常边界只记录阶段与异常类型，不输出可能含业务数据的异常消息。关闭日志不会跳过模型回调、记忆写入或保存。Java 17 控制台如出现路径乱码，可在主类前加 `-Dfile.encoding=UTF-8` 并使用 UTF-8 终端。

为了保持无 JSON 依赖，示例把 `recall(..., 3)` 返回的完整 JSON 作为低信任 user 消息加入原 prompt，不改系统消息。附加消息超过 4,000 个 Unicode 码点时整体跳过，绝不截断 JSON 字符串。这是字符预算，不是 token 预算。生产应用通常用自己的 JSON 库提取条目的 `text`，按模型的 token 预算选择完整记录；历史文字始终是资料，不应提升为系统指令。将历史标记为低信任不能独自保证抵抗提示注入。

所有 SDK 调用都是同步的。本例串行执行并显式关闭唯一 engine，不演示异步调度、取消、跨请求缓存或多进程共享。响应式服务应使用受控工作线程；同一实例方法串行化不代表整个“召回—模型—写入”业务回合具有事务性。

## 最小自查

1. 默认 legacy：两次 `Local model callback` 日志，最后输出 `Model call count: 2`，没有 AME 打开日志。
2. 可用 native 下的 QuickStart：日志显示两次写入结果、召回与统计 JSON 的长度；SDK 仍返回完整 JSON，关闭重开后仍可召回中文和 emoji，可通过 SDK 返回值或保存的快照检查内容。`turns/events` 是当前图的事件数，`total_chars` 是所有事件文本的 UTF-8 字节数；它们从已恢复的图计算，正常保存重开后应保持一致。
3. 可用 native 下的 AME 模式：输出哈希目录与输入消息的角色、长度，第二次模型输入可包含第一次成功回合的历史 JSON，日志不展示其内容；随后保存并关闭。
4. 给 AME 模式指定一个不存在的 `ame.native.path`：应出现 WARNING 级别的 `[open]` 日志、两次模型调用，且使用原始 prompt。相同设置下 legacy 仍可运行，因为它不加载 native。

重复使用同一演示目录会继续追加消息；本例不自动删除目录、不去重、不执行真实模型质量评测或性能测试。更多接口字段和集成策略见 [API 参考](../docs/API.md) 与 [集成指南](../docs/INTEGRATION.md)。

## 验证记录

当前源码版本以 [java/pom.xml](../pom.xml) 为准，实际运行版本以交付 POM 或元数据为准。[Linux 验证记录](../docs/LINUX-VALIDATION.md) 保留当时坐标、Java 包和文件名对应的验证结论，不自动覆盖后续产物。流水线中 SDK smoke/boundary 通过不等于这两个集成示例已经运行；只有本次构建日志或验证记录中明确列出的实际执行项才算完成验证。

[RC1 历史验证](../docs/HISTORICAL-RC1-VALIDATION.md) 仅对应当时的产物，不能替代当前交付物的验收。这里的模型始终是本地固定回调，示例运行不证明真实模型回答质量、流式取消行为或生产性能。
