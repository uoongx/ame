# Linux x86_64 SDK RC2 验证记录

验证日期：2026-10-28。当前版本为 `io.ame:ame-java:3.0.0-rc.2`，classifier 为 `linux-x86_64`，Java 17+、JNI ABI 1。当前仅支持 Linux x86_64；旧版双平台结果保留在 [RC1 历史记录](HISTORICAL-RC1-VALIDATION.md)，历史包未改写。

## 当前交付物

流水线归档位于 `build/cloudbuild/artifacts/ame-java-linux-x86_64.tar.gz`，同目录有 `SHA256SUMS`。归档中的 JAR、POM、SO、元数据均有独立校验值。

| 文件 | SHA256 |
| --- | --- |
| `ame-java-3.0.0-rc.2-linux-x86_64.jar` | `3558f5143fc77a327ffd4d93057aef8b21a9c07cd6749ebbcb6fb0ba29a916f3` |
| `libame_jni.so` | `40fde29f5d719970a37a589c34ea614ad798b654cc9fecbd458aa570f6e21836` |
| `ame-java-linux-x86_64.tar.gz` | `92b8e5c0ddd3bbd5e86cc76c11a57fc9b82f4dc91aa43e17fb73ffd9f3cda696` |

JAR 内唯一原生库为 `io/ame/native/linux-x86_64/libame_jni.so`，已核对与本次新编译的 SO 逐字节一致。移除非 Linux 条件分支后，Linux SO 字节与此前已验证二进制相同；Java 加载器与 Maven 版本已更新，因此使用独立 RC2 构件。

## 构建验证

在 Debian 12 x86_64 容器中执行 `sh .cloudbuild/build.sh`，网络关闭，源码与已准备好的 Maven 缓存只读挂载，仅构建输出可写：

| 工具 | 版本 |
| --- | --- |
| JDK | Debian OpenJDK 17.0.20.1 |
| Maven | 3.9.11 |
| CMake | 3.25.1 |
| C++ | g++ 12.2.0 |

流水线全新编译 native 和 Java 源码、打包 classifier JAR，随后直接使用该 JAR 运行测试，未将未打包的 SDK classes 放入测试 classpath，也未配置 `ame.native.path`。Maven 的 `-DskipTests` 仅跳过 Surefire；本项目的三个测试 main 由脚本显式执行，失败会阻止产物归档。

| 验证项 | 结果 |
| --- | --- |
| `NativeLibraryLoaderTest`：14 个平台拒绝场景、3 个 x86_64 别名 | 通过 |
| `AmeEngineSmokeTest`：中文/emoji、锁、保存重开、失败关闭 | 通过 |
| `AmeEngineBoundaryTest`：损坏快照、Unicode 路径跨进程锁、并发关闭 | 通过 |
| `.cloudbuild/test_build.py`：4 项失败重试、并发锁、符号链接保护 | 通过 |
| 最终归档内外 SHA256 | 通过 |

CMake 另对 Windows、Darwin、Linux/aarch64 三个不支持目标执行配置负例，均在编译器检测前拒绝。平台拒绝测试同时覆盖默认加载和显式外置库路径；外置库不能绕过平台检查。

## 独立运行环境验证

将同一最终归档放入 Ubuntu 24.04.5 LTS x86_64 / Temurin 21.0.12.1 容器，在禁网条件下解包并核对全部 SHA256，再执行：

- 上述平台、smoke、boundary 三个测试 main，全部通过。
- `javac --release 17` 编译两个集成示例。
- QuickStart：写入两条事件，中文和 emoji 可召回，关闭重开后统计一致。
- legacy：配置不存在的 native 路径仍正常完成两次本地模型调用，不加载 AME。
- AME：第二轮提示词含第一轮真实历史，两次模型调用，无回退告警。
- 缺库回退：明确输出初始化告警，转 legacy，完成两次调用，未创建记忆目录。

这些示例使用合成数据和固定模型回调，没有真实模型、RAG 或外部业务请求。

## 兼容与证据范围

ELF 为 x86_64，所需最高 glibc 符号版本为 **2.36**；动态依赖为 `libm.so.6`、`libc.so.6`、`ld-linux-x86-64.so.2`。C++ / GCC 运行库静态链接，不动态依赖 libstdc++、libgcc_s 或 libjvm。glibc 2.36 是必要条件，不代表全部发行版和部署策略已验证；Alpine/musl、ARM64 和非 Linux 平台不支持。

本次流水线记录在 `build/cloudbuild/runs/20261028T032810Z-p7ww5wq6/`，独立运行环境日志在 `build/cloudbuild/rc2-runtime-validation/`。归档的 `metadata/` 包含构建环境、ELF、平台和 JNI 测试记录。

`java/Dockerfile` 已改为复用同一个 `.cloudbuild` 入口，但本次未端到端执行 Dockerfile 的镜像下载/系统依赖安装/导出流程。也未在公司流水线实跑、未升级或重测文旅 Agent 的 RC2 依赖、未进行 Linux 正式性能基准或生产部署。
