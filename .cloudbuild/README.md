# AME Linux 流水线构建

在仓库根目录执行：

```sh
sh .cloudbuild/build.sh
```

也可用脚本的绝对路径调用，不依赖流水线的当前目录或 `WORKSPACE` 变量。入口只构建 AME Java SDK 和 JNI，不启动 Agent、不调用模型、不发布 Maven 制品。失败返回非零退出码。

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
3. 在隔离目录复制 Java 源码，排除源码资源中的旧 DLL/SO/dylib。版本取自 `java/pom.xml`，通过 Maven 打包本次 SO。
4. 检查 JAR 中仅包含 `io/ame/native/linux-x86_64/libame_jni.so`，且与本次构建 SO 逐字节一致。
5. 显式运行 `NativeLibraryLoaderTest`、`AmeEngineSmokeTest`、`AmeEngineBoundaryTest`，分别验证平台拒绝策略、真实 JNI 与持久化边界。它们是 Java main，不能把 Maven 的 `Tests run: 0` 当作 JNI 验证。测试 classpath 仅包含测试类和最终 JAR；移除测试子进程的 JVM 注入变量，确保使用 JAR 内 native 资源。
6. 全部通过才生成交付归档和校验文件。归档包含工具链、ELF、三个测试日志及构建状态，不包含测试记忆数据或 Maven 缓存。

同一 checkout 通过文件锁禁止并行构建；并行任务使用各自的工作区。失败重试会清除两个固定发布文件，防止上传旧成功产物；历史 run 的日志和临时构建树保留。流水线负责其工作区的常规回收。脚本不递归删除工作区，也不覆盖 `java/dist/` 中的历史交付物。

## 产物归档

只归档以下两个文件：

```text
build/cloudbuild/artifacts/ame-java-linux-x86_64.tar.gz
build/cloudbuild/artifacts/SHA256SUMS
```

若公司 `hwcloud_artifact` 当前任务使用“全路径 Perl 正则、每项只匹配一个文件”的约定，可配置以下两项；需在实际任务确认插件语义：

```text
.*/build/cloudbuild/artifacts/ame-java-linux-x86_64\.tar\.gz$
.*/build/cloudbuild/artifacts/SHA256SUMS$
```

固定归档名方便平台收集；压缩包内部带 POM 版本：

```text
ame-java-<version>-linux-x86_64.jar
ame-java-<version>.pom
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

解包校验：

```sh
# 在下载两个流水线工件的目录执行
sha256sum -c SHA256SUMS
mkdir sdk
tar -xzf ame-java-linux-x86_64.tar.gz -C sdk
(cd sdk && sha256sum -c SHA256SUMS)
```

构建失败时查看控制台输出的 `build/cloudbuild/runs/<本次目录>/logs/` 和 `status.json`。平台可将该目录作为失败诊断保留；不要用递归匹配整个 `build/` 的方式收集正式制品。

## Agent 依赖配置

从 `3.0.0-rc.2` 起仅维护 **Linux x86_64 classifier 构件**，Maven 默认打包与流水线命名一致。新版本不覆盖历史 RC1 交付物。在解包目录安装：

```sh
mvn install:install-file \
  -Dfile=ame-java-3.0.0-rc.2-linux-x86_64.jar \
  -DpomFile=ame-java-3.0.0-rc.2.pom \
  -Dclassifier=linux-x86_64
```

Linux Agent 引用：

```xml
<dependency>
  <groupId>io.ame</groupId>
  <artifactId>ame-java</artifactId>
  <version>3.0.0-rc.2</version>
  <classifier>linux-x86_64</classifier>
</dependency>
```

公司 Maven 发布步骤同样需要传递 classifier，并遵循仓库的版本不可覆盖规则。脚本不会自动 `install` 或 `deploy`。旧双平台 ZIP 打包入口已移除；`java/Dockerfile` 也复用本脚本，不再维护独立打包流程。

Java 接口、可插拔上下文与回退用法见 [SDK 集成指南](../java/docs/INTEGRATION.md)。

## 本地验证记录

2026-11-02 已在取消默认 glibc 上限并拆分构建阶段后重新完整执行入口，11 项策略/生命周期测试、17 项 C++ CTest、最终 JAR 的三个测试 main 全部通过。同一归档还在独立 Ubuntu 24.04/JDK21 环境通过 SDK 验证；实际 glibc 依赖随本次 Debian 12 工具链仍为 2.36，`max_glibc` 为 `null`。校验值与日志位置见 [Linux 验证记录](../java/docs/LINUX-VALIDATION.md)。

2026-10-28 已针对 Linux 专用 RC2 在 Debian 12 x86_64 容器中完整执行此入口：OpenJDK 17、g++ 12.2、CMake 3.25.1、Maven 3.9.11，网络关闭、Maven 缓存只读。原生编译、classifier JAR 打包、平台拒绝测试、两个真实 JNI 测试和归档内外 SHA256 校验均通过；SO 所需最高 glibc 符号版本为 2.36。同一归档还在独立 Ubuntu 24.04/JDK21 容器通过 SDK 和示例验证，详见 [Linux 验证记录](../java/docs/LINUX-VALIDATION.md)。公司平台门禁和 Dockerfile 镜像构建流程尚未实跑。

构建入口另有 7 项 glibc 策略检查和 4 项 Linux 生命周期检查，覆盖默认无上限、显式上限、符号版本数值比较、缺失元数据，以及编译失败时的发布文件清理、FAILED 诊断、并发锁和符号链接保护。可执行：

```sh
python3 -B .cloudbuild/test_build.py
```
