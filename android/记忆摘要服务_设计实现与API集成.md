# 记忆摘要服务：设计实现 · API · 集成文档

> 版本：dev-4.0（Android demo 应用）｜ 代码：`app/src/main/java/com/ame/memory/MemorySummaryService.java`
> 依赖：`MiniMaxClient`（LLM，MiniMax-M3）、`AmeEngine`（AME 图引擎 JNI）、org.json。

## 摘要格式

`MemorySummaryService.summarize(source, question)` 调用现有模型将材料整理为 L1 简述、L2 详情和联想词。L3 直接取自原文。
`storeToAme(summary, ts)` 将完整 JSON 作为一条事件写入，使用 `speaker=summary` 和 `MEMSUM:` 前缀。
对已有快照，调用方可使用以下代码选择注入层级。

```java
MemorySummaryService service = new MemorySummaryService(engine, sessionId);
List<MemorySummaryService.Summary> items = service.fetchFromAme(question, 5);
String brief = service.buildContext(items, 1);
String detail = service.buildContext(items, 2);
String original = service.buildContext(items, 3);
```

`Summary` 为只读值，调用方可通过 `getL1()`、`getL2()`、`getL3()` 和 `getKeywords()` 读取，原先直接读写字段的代码需要迁移。手工构造使用 `new Summary(l1, l2, keywords, l3)`，构造时复制联想词。

级别 0 或空集合生成空上下文。L1 上限为 60 个 UTF-16 单元，L2 为 300，联想词最多 6 个。
`fetchByKeywords(terms, limitPerKeyword)` 返回按联想词查询的摘要，按内容去重。

## 构建与验证

在 Android 项目的 `local.properties` 中设置 `SUMMARY_MEMORY=true` 后重新构建。
打开时，导入的每个方案块、成功完成的一轮对话分别保存为一条摘要事件。模型失败时用原文片段生成 L1/L2，L3 保留原文。
默认值为 `false`，继续写原格式的事件，同时能够读取已有的摘要。新旧事件可在同一快照中并存。

`FollowupGate` 对首问和新话题使用 L1，对追问使用 L2 并用至多 6 个联想词扩检，每个词至多取 3 条。
L1/L2 每次装配最多 1500 个 UTF-16 单元，截断时保护代理对。L3 只在显式请求第三级时返回。
每条召回事件按 `speaker=summary` 和 `MEMSUM:` 前缀共同识别。普通对话里的同名前缀仍作为普通文本处理。

`ame_e2e_summary` 为离线 C++ 检查，验证摘要事件的写入、说话人和召回。
Android Java 层的离线回归覆盖摘要生成、旧事件兼容、追问判定与不合法摘要的降级。

Java 离线验证使用 Java 17，设置 `JSON_JAR` 指向本地 `json-20240303.jar` 并执行 `bash android/tests/run-summary-tests.sh`。
