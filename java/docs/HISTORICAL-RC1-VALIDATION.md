# 历史交付记录：3.0.0-rc.1

本文保留旧版双平台产物的实际验证证据，不代表当前版本的平台支持。当前 Linux x86_64 版本见 [Linux 验证记录](LINUX-VALIDATION.md)。

# Windows / Linux x86_64 统一包验证记录

验证日期：2026-10-27。当前集成候选版为 `io.ame:ame-java:3.0.0-rc.1`。此记录仅对应以下文件内容，交付时应同时核对版本与 SHA256。

| 文件 | SHA256 |
| --- | --- |
| `ame-java-3.0.0-rc.1.jar` | `ee581458aae1f868124409ac7c02bb5eb825447c3df2090336cb2905d7c58bfb` |
| `libame_jni.so` | `40fde29f5d719970a37a589c34ea614ad798b654cc9fecbd458aa570f6e21836` |

统一 JAR 包含 Windows DLL 和 `io/ame/native/linux-x86_64/libame_jni.so`。本次通过 Maven 将版本改为 `3.0.0-rc.1`，内嵌 POM、pom.properties 与交付 POM 一致；与此前统一 SNAPSHOT 包相比，仅两个 Maven 元数据文件内容变化，所有 Java class、DLL 和 SO 字节完全一致（另增加一个空目录条目）。旧统一包 SHA256 为 `1072f46796039ad73319a744dbfa8a482974e35eb17a35922ccd8c637c97a9b8`。两个平台都重新通过 RC JAR 内资源自动加载验证，未设置 `ame.native.path`。SDK 是应用依赖库，不是可用 `java -jar` 启动的服务。

## 本版运行条件

- Java 17+，JVM 与原生库均为 x86_64；实际运行验证使用 Java 17。
- Windows x64，或具有兼容 **glibc 2.36+** 的 Linux x86_64。glibc 版本是必要条件，不代表已经验证所有发行版、容器安全策略和文件系统。
- Linux 动态依赖为 `libm.so.6`、`libc.so.6`、`ld-linux-x86-64.so.2`。已静态链接 GCC 的 C++/异常运行库，不要求外置 `libstdc++.so`、`libgcc_s.so` 或 `libjvm.so`。
- Java 临时目录须可写且允许加载原生库；否则按 [集成指南](INTEGRATION.md) 使用可信的外置库路径。应用数据目录须可写，并支持 SDK 所需的独占文件锁。
- **本包不适用于默认 glibc 2.35 的 Ubuntu 22.04、较旧 glibc 的 CentOS 或 Alpine/musl。** 不含 Linux ARM64、Windows ARM64 或 macOS 库。其他目标需要分别构建和验证。

部署机可先执行 `uname -m`、`ldd --version` 和 `java -version` 检查架构及运行环境，然后运行 [QuickStart](../examples/README.md) 验证实际部署目录的加载、锁和持久化行为。

## 原生构建

本次使用缓存的 Debian 12 x86_64 Linux 工具链容器，执行仓库中的 `java/scripts/build-linux-native.sh`。源代码只读挂载，输出独立挂载，构建和测试容器禁用网络。

| 项目 | 实际值 |
| --- | --- |
| 系统 | Debian GNU/Linux 12 (bookworm), x86_64 |
| C++ | g++ 12.2.0-14+deb12u1 |
| CMake | 3.25.1 |
| JNI 头文件 | OpenJDK 17.0.20.1 Debian |
| 构建类型 | Release |
| 额外链接参数 | `-static-libstdc++ -static-libgcc -Wl,--exclude-libs,ALL` |
| 原生 ABI | 1 |

在安装了上述工具的 Linux 环境中，从仓库根目录执行：

```sh
sh java/scripts/build-linux-native.sh /absolute/ame-linux-output
```

脚本输出 `native-build/libame_jni.so`、Maven 可打包的 `native-build/resources/`，以及工具链、ELF、导出符号和 SHA256 记录。它检查 JNI 导出和外部运行库依赖，不代替 JVM 测试。headless JDK 使用 CMake 3.24+ 的 `FindJNI COMPONENTS JVM`，仅使用 JNI 头文件，不链接 JVM 动态库。

ELF 验证确认 `ELF64`、`Advanced Micro Devices X86-64`，完整导出 `JNI_OnLoad` 和 7 个 `Java_io_ame_AmeEngine_*` 入口。所需最高 glibc 符号版本为 `GLIBC_2.36`；静态链接 C++ 运行库不会降低 glibc 要求。

仓库另提供 Debian 12 / JDK 17 的 `java/Dockerfile`，可完成原生构建、Maven 打包和两个 JVM 测试。**此次实际执行的是上面的构建脚本和下述测试；没有端到端执行更新后的 Dockerfile。** 重新构建时软件仓库和镜像标签可能变化，应重新记录依赖、检查 ELF 并执行目标平台测试。

## 实际验证结果

| 环境 | Smoke | Boundary | 集成示例 |
| --- | --- | --- | --- |
| Windows 11 x64 / Microsoft JDK 17.0.16 | 通过 | 通过 | QuickStart、legacy、AME 通过 |
| Debian 12 x86_64 / OpenJDK 17.0.20.1 | 通过 | 通过 | 未在此环境单独运行 |
| Ubuntu 26.04.1 x86_64 / glibc 2.43 / Temurin 17.0.20 | 通过 | 通过 | QuickStart、legacy、AME 通过 |
| Ubuntu 22.04.5 x86_64 / glibc 2.35 / Temurin 17.0.20.1 | 原生库基线预期拒绝加载 | 未运行 | 未运行 |

前三行均已针对当前 RC JAR 重新执行。Ubuntu 26.04 使用独立的官方 Java 运行容器，没有复用编译器环境。Ubuntu 22.04 的兼容性反例来自此前统一 SNAPSHOT 包，明确报 `GLIBC_2.36 not found`；RC 包内 SO 字节与该基线相同，本次没有重跑这项负向检查，不属于该系统的成功支持记录。

Smoke/Boundary 覆盖真实 JNI 加载、中文与 emoji、Unicode 目录、保存重开、损坏快照保护、重复打开与关闭、关闭保存失败后释放资源、子进程目录锁和 native 并发关闭边界。集成示例使用合成数据和本地模型回调：QuickStart 保存前后统计一致；legacy 与 AME 各调用模型两次，AME 第二轮包含第一轮真实历史 JSON，无告警回退。

Linux 测试入口示例（先用 `javac --release 17` 编译仓库测试类到独立目录）：

```sh
java -cp /tests:/pkg/ame-java-3.0.0-rc.1.jar \
  io.ame.AmeEngineSmokeTest /tmp/ame-smoke
java -cp /tests:/pkg/ame-java-3.0.0-rc.1.jar \
  io.ame.AmeEngineBoundaryTest /tmp/ame-boundary
```

Windows 使用 `;` 分隔 classpath。本次正向运行测试均针对上表 SHA256 的 RC 统一 JAR，没有把未打包的 SDK classes 放入 classpath。

## 交付与证据

仓库交付目录为 `java/dist/3.0.0-rc.1/`，包含统一 JAR、POM、独立 Linux `.so` 和带文档、示例、校验值的 ZIP。此前的 Windows 单平台包和统一 SNAPSHOT 包保留在原位置。

ZIP 内 `metadata/` 收录构建工具版本、ELF 检查、镜像 ID、测试日志、源文件指纹及 `release-validation.json`；`SHA256SUMS` 用于核对包内文件。测试数据均为合成数据，没有包含业务快照或 Maven 缓存。

本次验证不涉及真实 LLM、文旅 Agent 生产部署、Linux 吞吐或时延基准。
