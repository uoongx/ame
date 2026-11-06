# AME Java SDK 接口参考

本文描述当前源码对应版本的 `com.huawei.convai:convai-ame-java:<version>` API（标准主 JAR，无 classifier），运行条件为 Linux x86_64、Java 17+，AME JNI ABI **2**。`<version>` 表示所选 SDK 版本：当前源码版本以 [java/pom.xml](../pom.xml) 的顶层 `version` 为准，已出包版本以交付的 `convai-ame-java.pom` 或 `metadata/build.json` 为准。接口依据当前 `AmeEngine.java`、JNI 实现及底层引擎核实。

Java 包已从 `io.ame` 迁移至 `com.huawei.convai.ame`，JNI 资源路径同步迁移；调用方须更新 import 并重新编译。Maven groupId/artifactId、Linux 平台限制与 `ame.native.path` 属性保持不变，升级时应核对所选版本的接口文档。

入口是 `com.huawei.convai.ame.AmeEngine`，它实现 `AutoCloseable`。调用在当前 JVM 进程内同步执行；SDK 没有 HTTP endpoint、服务端口、模型 key 或远程存储配置。返回的记忆证据由应用解析并交给自己的模型，SDK 不生成回答。

通用接入方式见 [集成指南](INTEGRATION.md)，可编译示例见 [examples](../examples/README.md)，构建和分发见 [SDK README](../README.md)。

## 1. 公共方法一览

**4.0 整合分支的 `AmeEngine` 声明了 11 个公共方法。** `dev-3.0` 的 6 个方法继续可用。4.0 新增 `recallWithTenant`、`blockEvent`、`unblockEvent`、`deleteEvent` 和 `clearMemory`。下方第 2–7 节说明原有方法，第 8–12 节逐项说明新增方法。

```java
public static AmeEngine open(java.nio.file.Path directory);

public synchronized String rememberTurn(
    String speaker, String text, long epochSeconds,
    String messageId, String sessionId);

public synchronized String recall(String query, int limit);

public synchronized String stats();
public synchronized void save();
public synchronized void close();

public synchronized String recallWithTenant(String query, int limit, AmeEngine tenant);
public synchronized boolean blockEvent(String eventId);
public synchronized boolean unblockEvent(String eventId);
public synchronized boolean deleteEvent(String eventId);
public synchronized void clearMemory();
```

`rememberTurn`、`recall`、`recallWithTenant`、`stats` 返回 JSON **字符串**，使用应用自己的 JSON 库解析。JSON 对象字段顺序不构成契约；事件和实体 UID 应视为不透明字符串，不应依赖其前缀或生成格式。

所有 Java `String` 参数均不可为 `null`。JNI 显式转换 UTF-16 与普通 UTF-8，支持中文、emoji 等 Unicode 字符；不成对的 UTF-16 surrogate 会被拒绝。文本中的 NUL 可保留并在 JSON 中转义为 `\u0000`，目录路径不允许 NUL。

## 2. `open(Path directory)`

打开一个目录中的引擎，返回需要由调用方关闭的 `AmeEngine`。

| 参数 | 类型 | 语义与校验 |
|---|---|---|
| `directory` | `Path` | 非 `null`。先转为绝对路径并执行 `normalize()`；不存在的目录会创建。需要读取快照、创建锁文件和保存快照的权限。 |

打开过程：

1. 校验当前 JVM 为 Linux x86_64，加载 native library，并检查 JNI ABI 版本。显式外置路径也需要通过平台校验。
2. 创建目录，取得该目录 `.ame.lock` 的操作系统排他锁。
3. 若 `graph.json` 不存在，得到空引擎；若存在，则读取并校验后恢复图、实体槽位和别名。

空文件、非法 JSON、不合法的 Unicode、缺少必需图结构、未知节点/关系类型、悬空边等会使打开失败。**已有坏快照不会被当作空库，也不会在打开失败时被覆盖。** 打开失败可能已创建目录或 `.ame.lock` 文件，但不会返回可用实例。

同一目录不能被另一个引擎实例或进程同时打开，包括只想读取的实例。当前 API 没有只读模式；重复打开或目录不可写通常表现为 `UncheckedIOException`。

```java
import com.huawei.convai.ame.AmeEngine;
import java.nio.file.Path;
import java.util.logging.Logger;

try (AmeEngine memory = AmeEngine.open(Path.of("data", "memory", "user-123"))) {
    Logger.getLogger("com.huawei.convai.ame.integration").info(memory.stats());
}
```

应用负责从可信身份选择目录；`speaker`、`messageId` 和 `sessionId` 都不能替代用户隔离。不同用户若写入同一个目录，检索可访问该目录中的所有记忆。

## 3. `rememberTurn(...)`

将一条对话文本写入当前内存图，返回写入结果 JSON。每次正常调用生成一个新的事件，另外可能生成实体、提及、关键词等节点及关系。

| 参数 | 类型 | 语义与校验 |
|---|---|---|
| `speaker` | `String` | 非 `null`，可为 `""`。非空时作为主体实体加入解析，并记录在事件属性中；没有 `user`/`assistant` 等角色枚举校验。同名说话人会参与实体复用和消歧，不是鉴权身份。 |
| `text` | `String` | 非 `null`、不可为长度 0 的字符串。当前实现不拒绝仅含空白的文本，也没有公开的长度上限；应用应自行定义业务输入长度和空白处理。 |
| `epochSeconds` | `long` | UTC Unix 时间戳，单位为**秒**。允许 `0` 到 `9_007_199_254_740_991L`；负数或超过上界会失败。不要传 `System.currentTimeMillis()`。 |
| `messageId` | `String` | 非 `null`，可用 `""` 表示未提供。非空值保存为内部事件属性 `dia_id`，是来源标记。 |
| `sessionId` | `String` | 非 `null`，可用 `""` 表示未提供。非空值保存为内部事件属性 `session_id`。 |

**时间戳 `0` 的实际含义是“不覆盖规则提取出的时间”。** 文本包含“昨天”“前天”“上个月”“去年”等时，规则提取器可能使用相对时间；否则通常使用当前时间。若业务需要精确记录本次发言时间，应显式传入 `Instant.now().getEpochSecond()`，而不是 `0`。正数时间戳覆盖提取时间；上界校验不等于验证日期合理性。

```java
String resultJson = memory.rememberTurn(
    "user",
    "用户喜欢安静的展览，也喜欢青铜器 😊",
    java.time.Instant.now().getEpochSecond(),
    "message-42",
    "session-7");
```

返回结构示例（UID 为示意值）：

```json
{
  "code": 0,
  "event_id": "generated-event-uid",
  "resolved_entities": [
    {"surface": "user", "uid": "resolved-entity-uid"}
  ]
}
```

| 字段 | JSON 类型 | 含义 |
|---|---|---|
| `code` | number | 正常返回时为整数 `0`。当前 JNI 对引擎非成功结果抛异常，不通过非零 `code` 返回失败。 |
| `event_id` | string | 本次创建的事件 UID。 |
| `resolved_entities` | array | 本次解析得到的实体映射，可为空数组；每项有 `surface` 和 `uid` 两个字符串字段。可能包含 `speaker` 及文本中被规则识别的实体，不保证识别所有实体。 |

正常返回仅表示内存写入完成；**不会自动调用 `save()`**。持久化需要显式 `save()` 或 `close()`。

`messageId` 不提供幂等或重复检查。用相同 `messageId` 重试仍可能新增事件，换一个 `sessionId` 也不会创建独立的检索分区。需要去重时，由应用在写入前维护自己的幂等策略。SDK 不提供事务或回滚；异常、超时后不能一概假设未写入，也不应直接盲目重试。

## 4. `recall(String query, int limit)`

从当前引擎中检索相关事件，返回 JSON 数组。新写入且尚未保存的事件也可被同一实例检索。

| 参数 | 类型 | 语义与校验 |
|---|---|---|
| `query` | `String` | 非 `null`，必须通过 `!query.isBlank()` 校验。不会自动去掉查询首尾空白。 |
| `limit` | `int` | 最多返回的事件数量，范围 **1–100**，包含端点。不是分页大小、候选数限制或检索计算预算。 |

```java
String evidenceJson = memory.recall("喜欢什么样的展览", 5);
```

返回结构示例，时间戳和分数仅展示类型与形状，不是稳定的预期结果：

```json
[
  {
    "event_id": "generated-event-uid",
    "text": "用户喜欢安静的展览，也喜欢青铜器 😊",
    "ts": 1793064823,
    "score": 0.708012345,
    "explain": "直接命中（种子节点）"
  }
]
```

| 字段 | JSON 类型 | 含义 |
|---|---|---|
| `event_id` | string | 被召回事件的 UID，可与写入结果关联。 |
| `text` | string | 存储的事件文本。 |
| `ts` | number | 事件的 UTC Unix 秒时间戳。 |
| `score` | number | 本次查询的排序分数，较大者优先。不是匹配概率或模型置信度。 |
| `explain` | string | 图检索路径的人类可读说明；是普通字符串，不是 JSON 对象或数组。 |

无结果返回 `[]`，不抛“未找到”异常。返回数量可能少于 `limit`；当前重排器会按**完全相同的文本**去重，因此即使多次写入得到不同事件 ID，检索结果也可能只显示其中一条。它不是列举全部历史的 API。

当前 JNI 固定使用深度 2 的图扩散、默认过滤器和开启的解释输出。Java API 没有 session/user/time 过滤参数、分页游标、分数阈值、深度配置或 `explain` 开关。`speaker`、`messageId`、`sessionId` 也不包含在此返回 JSON 中。

### `explain` 的形式

直接种子命中时，当前实现使用 `直接命中（种子节点）`。经过图关系时，形式类似：

```text
实体显示名 -[INVOLVES]-> 事件显示名
节点甲 -[关系名]-> 节点乙  →  节点乙 -[关系名]-> 节点丙
```

多段用 `  →  ` 连接，显示名可能被截短，缺失节点用 `?`。解释中的显示名不是完整事件正文；调用方应使用 `text` 作为证据正文，不能依赖解释字符串解析出稳定的结构化路径。JNI 没有输出内部 `path` 或各子分数。

### `score` 的当前计算方式

当前默认排序融合：

```text
score = 0.5 × graph + 0.3 × context + 0.2 × decay × importance
```

- `graph`：事件图扩散能量除以本次候选事件的最大能量。
- `context`：事件向量与查询向量的余弦相似度，负值截为 0。
- `decay`：以 90 天为半衰期的时间衰减，未来时间按年龄 0 处理。
- `importance`：存储事件的重要性；普通写入当前初值为 0.5。

该 Java API 使用内置规则提取器和默认 128 维字符 n-gram 哈希向量，没有外部模型调用，也没有 Java 层的提取器/embedding 注入入口。图结构、候选集和当前时间变化都可能影响分数；不要将它当作跨查询、跨数据集可比较的概率，也不要依赖固定的 `[0,1]` 契约。并列结果没有稳定次序承诺。

## 5. `stats()`

返回当前内存图的统计 JSON，无参数。

```json
{"turns":0,"total_chars":0,"events":0,"nodes":0}
```

上例是新建空库的结果。各字段均为 JSON number，当前表示整数计数：

| 字段 | 实际统计口径 |
|---|---|
| `turns` | 事件节点数，与 `events` 相同。一次 `rememberTurn` 正常产生一个事件；不是“一问一答”对数。 |
| `total_chars` | 所有事件 `text` 的 **UTF-8 字节数之和**。尽管字段名含 `chars`，它不是 Java `String.length()`、Unicode 码点数或 token 数。 |
| `events` | 事件节点数，包含重复文本产生的不同事件。 |
| `nodes` | 图中全部节点数，包括事件、实体、关键词、提及等；通常大于事件数。 |

例如，将上面 53 个 UTF-8 字节的示例文本写入两次，即使使用相同 `messageId`，`turns/events` 都增加 2，`total_chars` 增加 106；`nodes` 的变化依赖实体复用和规则提取，不能由轮数直接推算。

## 6. `save()`

同步持久化当前实例的全部状态，无参数、无返回值。

当前保存顺序是：构造包含 `graph`、`slots`、`aliases` 的完整 JSON → 写同目录 `graph.json.tmp` → flush 并关闭文件 → 替换 `graph.json`。失败时尝试清理临时文件。

这是**全量快照**，不是增量写入，也没有后台保存。大图保存的 CPU、内存和文件 I/O 会占用调用线程。`save()` 失败时通常抛 `UncheckedIOException`，实例仍保持打开；调用方可在排除问题后决定是否重试。

不要把“临时文件替换”理解为事务/WAL、跨机器一致性或任意断电场景下的持久性保证。当前 POSIX 分支没有额外执行文件和父目录 `fsync`；打开时也不会自动从遗留 `.tmp` 恢复。不要直接修改正在使用的快照或锁文件。

## 7. `close()`

同步保存并释放 native 引擎及目录锁。推荐使用 `try-with-resources`。

- 正常关闭会再次执行全量保存，即使之前刚调用过 `save()`，或期间只读没有写入；当前没有 dirty 检查。
- 关闭后，使用有效参数调用该实例的读写、查询、清理和保存方法均抛出 `IllegalStateException`。
- 重复调用 `close()` 不做任何事。
- **关闭时保存失败，句柄和锁仍会释放，对象仍保持关闭。** 不能用该对象再次 `save()`；未成功持久化的修改可能丢失。调用方必须观察关闭异常。
- SDK 没有依赖 finalizer/Cleaner 自动关闭实例。仅丢弃 Java 对象引用不会替你保存并释放 native 注册句柄。

`try-with-resources` 在业务代码已经抛异常时，会把关闭异常保留为 suppressed exception；这样可以同时保留原始失败和保存失败。

## 8. `recallWithTenant(String query, int limit, AmeEngine tenant)`

同时查询当前实例和宿主授权的租户实例，返回与 `recall` 相同的 JSON 数组。两个库的候选事件按 `event_id` 去重、按分数降序合并，最终最多返回 `limit` 条。没有匹配时返回 `[]`。共享范围仅限本次调用，随后调用 `recall()` 只查询当前实例。

`query` 和 `limit` 沿用 `recall` 的约束。两个实例都必须打开。`tenant` 为 `null` 时抛出 `NullPointerException`，传入当前实例自身时抛出 `IllegalArgumentException`，使用已关闭的实例时抛出 `IllegalStateException`。调用方负责根据已认证身份选择租户目录。

```java
try (AmeEngine visitor = AmeEngine.open(visitorDirectory);
     AmeEngine tenant = AmeEngine.open(authorizedTenantDirectory)) {
    String evidence = visitor.recallWithTenant("青铜器展厅在哪里", 5, tenant);
}
```

## 9. `blockEvent(String eventId)`

屏蔽当前库中的一条事件。屏蔽立即生效，两种召回方法查询该库时都会跳过它。`stats()` 仍会统计这条事件。

三个条目操作都接受写入或召回结果中的 **`event_id`**。参数为 `null` 时抛出 `NullPointerException`，空字符串抛出 `IllegalArgumentException`。传入未知 ID 或非事件节点 ID 返回 `false`。在写入该事件的实例上调用，修改后用 `save()` 或正常 `close()` 持久化。

```java
if (memory.blockEvent(eventId)) {
    memory.save();
}
```

事件存在时返回 `true`，重复屏蔽也返回 `true`。3.0 召回会忽略屏蔽标记。若回退后仍须隐藏这些事件，可在新版本删除它们并保存。若要保留原始数据，由宿主过滤旧版召回的事件及相关解释文本；其他事件的 `explain` 也可能引用被屏蔽事件的名称。

## 10. `unblockEvent(String eventId)`

解除当前库中该事件的屏蔽，使其可以再次参与召回。事件存在时返回 `true`，包括它本来就可见的情况。已删除或未知的事件返回 `false`。例如，调用 `memory.unblockEvent(eventId)` 后，下一次查询即可按正常排序召回它。

## 11. `deleteEvent(String eventId)`

删除当前库中的事件、直接关联的边以及以该事件为来源的实体档案槽位。首次成功删除返回 `true`，再次删除同一 ID 返回 `false`。独立写入的摘要、其他库中的事件和调用方已经取得的 JSON 副本保留各自的生命周期。

```java
if (memory.deleteEvent(eventId)) {
    memory.save();
}
```

## 12. `clearMemory()`

清空当前实例的记忆图和实体档案，保留配置的词别名。无参数，返回 `void`。**清理范围是传给 `open(path)` 的整个目录所对应的库**。多个 `sessionId` 共用这个库时，所有会话一起清空。单独打开的租户共享库有自己的数据。

```java
memory.clearMemory();
memory.save();
```

实例在清空后仍可使用。`save()` 将清空结果写入快照，下次打开同一目录时从空图开始。

## 13. 异常契约

公共方法没有声明 checked exception。召回无匹配时返回 `[]`，三个按 `eventId` 操作的方法找不到目标事件时返回 `false`。参数、生命周期和读写错误通过 Java 异常报告。下面列出主要异常类型，异常消息文本可能随版本变化。

| 类型 | 常见触发条件 |
|---|---|
| `NullPointerException` | `directory`、`tenant` 或任一 String 参数为 `null`。 |
| `IllegalArgumentException` | 空 `text` 或 `eventId`、空白 `query`、`limit` 或时间戳越界、租户与当前实例相同、不成对的 UTF-16 surrogate 等。非法 `Path` 构造还可能抛其子类 `InvalidPathException`。 |
| `IllegalStateException` | 实例已关闭、native 引擎返回非成功状态或其他原生运行错误。 |
| `java.io.UncheckedIOException` | `open/save/close` 的 I/O 失败；包括目录不可写、锁冲突、已有快照损坏或不符合当前结构校验、保存/替换失败。底层 `IOException` 保留在 cause 中。 |
| `UnsatisfiedLinkError` | native 库缺失、路径不合法、系统/架构或依赖不匹配、加载失败、JNI ABI 入口缺失或版本不匹配。它是 `Error`，普通 `catch (Exception)` 捕获不到。 |
| `OutOfMemoryError` | Java/native 分配失败等。不能把这类错误当作可无条件重试的业务失败。 |

同时存在多种无效条件时，异常的检查顺序取决于方法。JVM、文件系统安全策略等还可能抛出各自的运行时异常。

JSON 数字内部使用 `double`。当前序列化器对绝对值小于 `10^15` 的整数按整数输出，其余按约 9 位有效数字输出；普通现实时间戳可按秒读取，但不要将参数上界理解为任意 `long` 数值都可无损往返的保证。

## 14. 线程、锁与取消

同一引擎的实例方法通过 `synchronized` 串行化；JNI 还有每实例 mutex 与共享句柄生命周期保护。同一实例上读、写、保存、关闭不会并行执行。不同目录的独立实例可以由应用并行调用；SDK 没有应用层的实例缓存或用户路由器。

每个打开的实例持续持有 `.ame.lock` 的操作系统排他锁，直到关闭或进程退出。磁盘上残留锁文件本身不代表仍被占用。该锁用于协调使用同一约定的引擎，不是租户鉴权或分布式存储协议；网络文件系统需要单独验证其文件锁和替换语义。

所有 API 都是阻塞调用；响应式服务器应安排到工作线程。API 没有 timeout、取消 token 或 interrupt 回调。`Thread.interrupt()`、Future 取消、Reactor timeout 都不保证立即停止正在执行的 C++ 操作，超时后写入或保存仍可能完成。调用 `close()` 会等待已进入的同实例操作结束，也不是强制停止手段。

图中的已有节点和边可以恢复，但加载不会恢复所有运行时状态。例如新实例的写入器不会从快照重建上一条事件的内部链尾，因此不承诺关闭重开后新写入自动接续原实例的 `NEXT` 链。需要连续会话写入时应复用引擎，避免每个请求都打开/关闭。

## 15. Java 版本、native ABI 与平台

Java 字节码目标版本是 **17**；AME 的私有 `nativeVersion()` 返回的兼容性版本必须为 **2**，每次 `open()` 都会校验。这里的 ABI 2 不等于 Maven 版本号；底层 `JNI_OnLoad` 使用 JNI 1.8 常量也不代表本 JAR 可运行于 Java 8。不要混用 Android JNI 桥或其他版本的 native 库。

初次加载顺序：

1. 校验 JVM 为 Linux x86_64；`os.arch` 接受 `amd64/x86_64/x64`。不支持的平台抛 `UnsatisfiedLinkError`，即使设置外置路径也不会跳过此检查。
2. 若设置 JVM 系统属性 `ame.native.path`，只尝试这个 SO 文件；相对路径会转为绝对路径。空值、目录、错误文件不会静默回退到 JAR 资源。
3. 否则从 `/com/huawei/convai/ame/native/linux-x86_64/libame_jni.so` 提取到 `java.io.tmpdir` 下的独立目录，再 `System.load`。运行环境需要允许临时文件写入与 native 加载。

```sh
java -Dame.native.path=/absolute/path/libame_jni.so -jar your-agent.jar
```

该属性须在首次 `open()` 前设置；同一类加载器生命周期只加载一次库，之后改属性不会切换已加载的库。JNI 导出符号随 Java 类名迁移，因此外置 SO 也须替换为同次构建产物；ABI 2 不兼容 `io.ame` 旧库。SDK 不从自定义环境变量或 HTTP 配置读取此值，也不自动下载缺失的库。

交付文件为 `build/cloudbuild/artifacts/convai-ame-java-<version>.jar` 及同目录的 `convai-ame-java.pom`，版本从这份交付 POM 或归档元数据读取，使用上述标准 Maven 坐标引用，不带 classifier。固定的只是交付 POM 文件名，groupId/artifactId 沿用上述坐标，Maven 仓库仍使用标准带版本布局。构建入口仍是 `sh .cloudbuild/build.sh`，只打包本地产物，不自动 `install` 或 `deploy`；诊断归档和校验步骤见 [SDK README](../README.md)。Linux 运行时还必须满足实际 SO 的 glibc 要求；构建默认不设置版本上限，仅在显式配置 `AME_MAX_GLIBC` 时检查。实际要求以本次归档 `metadata/build.json` 的 `required_glibc` 及测试日志为准，[Linux 验证记录](LINUX-VALIDATION.md) 保留其记录的历史坐标、Java 包与产物证据。版本号和 Java 字节码目标都不能替代目标系统验证。

公共接口以本文列出的方法为准。C++ 内部的注册实体、精确查询、时间过滤、修改权重等能力没有因此自动成为 Java 公共接口；应用适配层的 provider、用户缓存和失败回退也不是 SDK 内置配置项。
