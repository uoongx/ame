# AME Linux 流水线构建

在仓库根目录执行：

```sh
sh .cloudbuild/build.sh
```

也可用脚本的绝对路径调用，不依赖流水线的当前目录或 `WORKSPACE` 变量。入口只构建并打包 AME Java SDK 和 JNI，生成可交给公司平台制品发布步骤的本地 JAR/POM；不启动 Agent、不调用模型，也不自动执行 Maven `install` 或远端 `deploy`。失败返回非零退出码。

## 公司平台配置

在公司的 Linux x86_64 构建任务中，检出本仓库后，将上述命令配置为构建步骤。使用已准备好下表工具的构建镜像，保留任务现有的代码检查和构建门禁。脚本不会自动安装系统包、下载工具链、切换源码或关闭证书验证。

| 构建环境 | 要求 |
| --- | --- |
| 系统 | Linux x86_64，glibc；按目标部署环境选择构建镜像，不固定 glibc 版本 |
| Java | JDK 17+，含 `java`、`javac` 和 JNI 头文件；Java 字节码固定为 release 17 |
| C++ | g++，支持 C++17，含可静态链接的 libstdc++ / libgcc；make |
| CMake | 3.24+，支持 headless JDK 的 JNI 查找 |
| 打包工具 | Maven 3.8+、Python 3.8+（只用标准库）、POSIX sh |
| 系统工具 | binutils（readelf、nm）、coreutils（sha256sum、readlink 等）；git 可选 |

构建节点需要能访问公司配置的 Maven 镜像，或预先准备完整的 Maven 本地缓存。镜像名称、资源池和归档插件版本由实际公司任务配置；本仓库未假定它们，也未提供未经平台验证的 YAML。

可配置以下环境变量；凭据通过平台注入的 Maven settings 管理，不写入脚本：

| 变量 | 默认值 | 用途 |
| --- | --- | --- |
| `JAVA_HOME` | PATH 中的 JDK | 设置时，优先使用该目录的 java / javac |
| `AME_BUILD_JOBS` | `2` | C++ 并行编译数，正整数 |
| `AME_MAVEN_SETTINGS` | Maven 默认 settings | 公司 settings.xml 的绝对路径；不会复制到产物 |
| `AME_MAVEN_REPO` | Maven 默认本地仓库 | 缓存的绝对路径，流水线应提供可写目录 |
| `AME_MAVEN_OFFLINE` | `false` | `true` 时传入 Maven `-o`，缺少缓存即失败 |
| `AME_MAX_GLIBC` | 未设置（无上限） | 可选：允许 SO 依赖的最高 glibc 符号版本，如 `2.28`；设置后超出即失败 |

例如在流水线环境配置好 `JAVA_HOME` 后运行：

```sh
AME_BUILD_JOBS=4 \
AME_MAVEN_SETTINGS=/ci/config/maven-settings.xml \
AME_MAVEN_REPO=/ci/cache/maven \
sh .cloudbuild/build.sh
```

默认不设置 glibc 上限，也不要求构建环境必须使用 2.36。脚本始终从实际 ELF 中提取最高 glibc 符号版本，写入 `metadata/build.json` 的 `required_glibc`；未设上限时 `max_glibc` 为 `null`。如需约束部署兼容性，可设置 `AME_MAX_GLIBC=2.28` 等目标版本（支持 `2.<minor>[.<patch>]`），超出即构建失败。

这个变量只检查产物，不会改变编译器的链接结果或自动降低 glibc 依赖。静态链接 C++ 运行库也不会消除 glibc 依赖。支持较老系统时，应使用与目标兼容的构建镜像/工具链，并在该目标环境运行测试。`java/Dockerfile` 默认 Debian 12 仅作为可复现的构建示例，可通过 `AME_BUILD_IMAGE` 指定满足上述工具要求的 Debian/Ubuntu 镜像；它不代表 SDK 强制要求 glibc 2.36，也不保证产物可运行于任意更低版本。此入口仅支持 x86_64；不支持 Alpine/musl、ARM64 或 Windows 交叉编译。

## 构建与验证

1. 检查平台、工具及参数，在 `build/cloudbuild/runs/` 创建本次唯一目录。
2. 调用 `java/scripts/build-linux-native.sh` 全新编译 JNI，检查 ELF64/x86_64、JNI 导出及动态依赖，记录实际 glibc 要求；仅在配置上限时执行 glibc 门禁。
3. 在隔离目录复制 Java 源码，排除源码资源中的旧 DLL/SO/dylib。坐标和版本取自 `java/pom.xml`，通过 Maven 将本次 SO 打进不带 classifier 的标准主 JAR。
4. 检查 JAR 中唯一 native 库为 `com/huawei/convai/ame/native/linux-x86_64/libame_jni.so`，且与本次构建 SO 逐字节一致；要求新包下的 `AmeEngine.class` 和 `NativeLibraryLoader.class` 存在，拒绝任何 `io/ame/` 遗留条目。同时核对新坐标下的内嵌 POM 原文和 `pom.properties` 的 groupId、artifactId、version。
5. 显式运行 `NativeLibraryLoaderTest`、`AmeEngineSmokeTest`、`AmeEngineBoundaryTest`，分别验证平台拒绝策略、真实 JNI 与持久化边界。它们是 Java main，不能把 Maven 的 `Tests run: 0` 当作 JNI 验证。测试 classpath 仅包含测试类和最终 JAR；移除测试子进程的 JVM 注入变量，确保使用 JAR 内 native 资源。
6. 全部通过才输出独立 JAR、配套 POM、诊断归档和校验文件。归档包含同名 JAR/POM、SO、工具链、ELF、三个测试日志及构建状态，不包含测试记忆数据或 Maven 缓存。

同一 checkout 通过文件锁禁止并行构建；并行任务使用各自的工作区。失败重试会先清理制品输出，防止上传旧成功产物；历史 run 的日志和临时构建树保留。流水线负责其工作区的常规回收。脚本不递归删除工作区，也不覆盖 `java/dist/` 中的历史交付物。

## 产物归档

当前版本输出以下四个文件。JAR/POM 可直接用于平台制品发布；压缩包用于保留诊断材料，构建脚本本身不执行发布：

```text
build/cloudbuild/artifacts/convai-ame-java-3.0.0-rc.2.jar
build/cloudbuild/artifacts/convai-ame-java-3.0.0-rc.2.pom
build/cloudbuild/artifacts/convai-ame-java-linux-x86_64.tar.gz
build/cloudbuild/artifacts/SHA256SUMS
```

外部 `SHA256SUMS` 覆盖 JAR、POM 和诊断归档。若公司 `hwcloud_artifact` 当前任务使用“全路径 Perl 正则、每项只匹配一个文件”的约定，可配置以下四项；需在实际任务确认插件语义，版本更新时同步 JAR/POM 的匹配规则：

```text
.*/build/cloudbuild/artifacts/convai-ame-java-3\.0\.0-rc\.2\.jar$
.*/build/cloudbuild/artifacts/convai-ame-java-3\.0\.0-rc\.2\.pom$
.*/build/cloudbuild/artifacts/convai-ame-java-linux-x86_64\.tar\.gz$
.*/build/cloudbuild/artifacts/SHA256SUMS$
```

固定诊断归档名方便平台收集；压缩包内部文件保留 POM 版本，SO 和元数据目录结构不变：

```text
convai-ame-java-<version>.jar
convai-ame-java-<version>.pom
libame_jni.so
metadata/build.json
metadata/toolchain.txt
metadata/elf-*.txt
metadata/*-version.log
metadata/NativeLibraryLoaderTest.log
metadata/AmeEngineSmokeTest.log
metadata/AmeEngineBoundaryTest.log
SHA256SUMS
```

`metadata/build.json` 包含以下制品标识；`platform` 字段标明 Linux 限定，不使用 Maven classifier 表达平台：

```json
{
  "group_id": "com.huawei.convai",
  "artifact_id": "convai-ame-java",
  "version": "3.0.0-rc.2",
  "java_package": "com.huawei.convai.ame",
  "classifier": null,
  "platform": "linux-x86_64"
}
```

解包校验：

```sh
# Run in the directory containing all four downloaded artifacts.
sha256sum -c SHA256SUMS
mkdir sdk
tar -xzf convai-ame-java-linux-x86_64.tar.gz -C sdk
(cd sdk && sha256sum -c SHA256SUMS)
```

构建失败时查看控制台输出的 `build/cloudbuild/runs/<本次目录>/logs/` 和 `status.json`。平台可将该目录作为失败诊断保留；不要用递归匹配整个 `build/` 的方式收集正式制品。

## Agent 依赖配置

当前坐标为 `com.huawei.convai:convai-ame-java:3.0.0-rc.2`，主 JAR 不带 classifier，仍只支持 **Linux x86_64**。Java 包已从 `io.ame` 迁移至 `com.huawei.convai.ame`，native 资源同步迁至 `com/huawei/convai/ame/native/linux-x86_64/libame_jni.so`；调用方须更新 import、反射类名或启动类名并重新编译。JNI ABI 仍为 1，外置库属性仍为 `ame.native.path`，但必须使用同次构建的 SO。历史坐标和交付物不覆盖。

迁包保留当前 GAV；`metadata/build.json` 的 `java_package` 字段用于区分 Java 包，消费时还应核对 JAR 校验值，避免误用本地缓存中的旧包产物。

本入口默认只执行打包。若仅需本机试用，可在仓库根目录手动安装配套 JAR/POM，以下命令不属于流水线默认步骤：

```sh
mvn install:install-file \
  -Dfile=build/cloudbuild/artifacts/convai-ame-java-3.0.0-rc.2.jar \
  -DpomFile=build/cloudbuild/artifacts/convai-ame-java-3.0.0-rc.2.pom
```

Linux Agent 引用：

```xml
<dependency>
  <groupId>com.huawei.convai</groupId>
  <artifactId>convai-ame-java</artifactId>
  <version>3.0.0-rc.2</version>
</dependency>
```

Gradle 使用 `implementation 'com.huawei.convai:convai-ame-java:3.0.0-rc.2'`，不添加 classifier。公司平台制品发布步骤使用独立 JAR/POM，并遵循仓库的版本不可覆盖规则；发布地址和权限继续由平台管理，本仓库不新增远端发布行为。旧双平台 ZIP 打包入口已移除；`java/Dockerfile` 也复用本脚本，不再维护独立打包流程。

Java 接口、可插拔上下文与回退用法见 [SDK 集成指南](../java/docs/INTEGRATION.md)。

## 本地验证记录

2026-11-03（迁包后）已在 Debian 12 x86_64 / OpenJDK 17 / Maven 3.9.11 环境中禁网完成全新构建。23 项构建回归、最终 JAR 的三个 SDK 测试 main、归档内外校验全部通过；JAR 中无旧 `io/ame/` 条目，两个 SDK 类、native 资源及 7 个 JNI 导出均使用新包名。独立 Maven 消费者使用相同 GAV 编译新包下的两个示例，`SdkQuickStart` 和可插拔示例的 legacy、AME、缺库回退三种场景均通过。元数据的 `java_package` 为 `com.huawei.convai.ame`；实际 SO 依赖 glibc 2.36，`max_glibc` 为 `null`。未实跑公司流水线或远端发布。

迁包后构建日志位于 `build/cloudbuild/runs/20261103T091435Z-wsi99gzd/`，消费与示例验证日志位于 `build/convai-namespace-20261103/`，当前产物校验值见 `build/cloudbuild/artifacts/SHA256SUMS`。以下记录保留迁包前的验证范围。

2026-11-03（迁包前）已在 Debian 12 x86_64 / OpenJDK 17 / Maven 3.9.11 环境中禁网完整执行入口，新坐标 `com.huawei.convai:convai-ame-java:3.0.0-rc.2` 的主 JAR、POM 和诊断归档已生成，Java 包仍为 `io.ame`。当时的 21 项构建策略、打包和生命周期测试全部通过；最终 JAR 的三个 SDK 测试 main、内外 SHA256 校验及 JAR/POM 字节一致性检查全部通过。另将同一 JAR/POM 安装到隔离的临时 Maven 缓存，独立消费者仅声明新坐标、无需 classifier，即可编译并运行旧包的 `io.ame.examples.SdkQuickStart`，完成 JNI 写入、召回和保存重开。实际 SO 依赖 glibc 2.36，`max_glibc` 为 `null`。该次未向远端 Maven 仓库发布，也未实跑公司流水线。

上述迁包前构建日志位于 `build/cloudbuild/runs/20261103T085435Z-c34uqhqp/`，独立消费验证日志当时保存在 `build/convai-package-20261103/validation.log`。这些记录仅对应当时的旧 Java 包；当前交付目录的同名产物可能被后续构建更新，须核对本次构建日志与 `metadata/build.json` 的 `java_package`。

以下记录对应旧坐标 `io.ame:ame-java:3.0.0-rc.2`、classifier `linux-x86_64` 及旧文件名，历史验证文件保持原样。

2026-11-02 已在取消默认 glibc 上限并拆分构建阶段后重新完整执行入口，11 项策略/生命周期测试、17 项 C++ CTest、最终 JAR 的三个测试 main 全部通过。同一归档还在独立 Ubuntu 24.04/JDK21 环境通过 SDK 验证；实际 glibc 依赖随本次 Debian 12 工具链仍为 2.36，`max_glibc` 为 `null`。校验值与日志位置见 [Linux 验证记录](../java/docs/LINUX-VALIDATION.md)。

2026-10-28 已针对 Linux 专用 RC2 在 Debian 12 x86_64 容器中完整执行此入口：OpenJDK 17、g++ 12.2、CMake 3.25.1、Maven 3.9.11，网络关闭、Maven 缓存只读。原生编译、classifier JAR 打包、平台拒绝测试、两个真实 JNI 测试和归档内外 SHA256 校验均通过；SO 所需最高 glibc 符号版本为 2.36。同一归档还在独立 Ubuntu 24.04/JDK21 容器通过 SDK 和示例验证，详见 [Linux 验证记录](../java/docs/LINUX-VALIDATION.md)。公司平台门禁和 Dockerfile 镜像构建流程尚未实跑。

上述旧构建的 11 项回归包括 7 项 glibc 策略检查和 4 项 Linux 生命周期检查，覆盖默认无上限、显式上限、符号版本数值比较、缺失元数据，以及编译失败时的发布文件清理、FAILED 诊断、并发锁和符号链接保护。当前构建脚本的回归检查可执行：

```sh
python3 -B .cloudbuild/test_build.py
```
