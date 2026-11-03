package com.huawei.convai.ame;

import java.nio.file.Files;
import java.nio.file.Path;
import java.util.Locale;
import java.util.logging.Logger;

/** Platform rejection checks; successful Linux JNI loading is covered by AmeEngineSmokeTest. */
public final class NativeLibraryLoaderTest {
  private static final Logger LOG = Logger.getLogger(NativeLibraryLoaderTest.class.getName());

  private NativeLibraryLoaderTest() {}

  public static void main(String[] args) throws Exception {
    if (args.length != 0) throw new IllegalArgumentException("Usage: NativeLibraryLoaderTest");
    String originalOs = System.getProperty("os.name");
    String originalArch = System.getProperty("os.arch");
    String originalNativePath = System.getProperty("ame.native.path");
    Path temporary = Files.createTempDirectory("ame-platform-rejection-");
    Path explicitLibrary = Files.createFile(temporary.resolve("libame_jni.so"));
    Path memory = temporary.resolve("must-not-be-created");
    String[][] rejected = {
        {"Windows 11", "amd64"}, {"Mac OS X", "x86_64"}, {"Darwin", "aarch64"},
        {"Linux", "aarch64"}, {"Linux", "arm64"}, {"Linux", "x86"},
        {"FreeBSD", "amd64"}
    };
    try {
      for (String[] target : rejected) {
        System.setProperty("os.name", target[0]);
        System.setProperty("os.arch", target[1]);
        System.clearProperty("ame.native.path");
        expectPlatformRejection(memory, target);
        // A real file ensures the platform gate runs before external path validation/loading.
        System.setProperty("ame.native.path", explicitLibrary.toString());
        expectPlatformRejection(memory, target);
      }
      check(!Files.exists(memory), "Rejected platforms must not create memory directories");
      for (String arch : new String[] {"amd64", "x86_64", "x64"}) {
        System.setProperty("os.name", "Linux");
        System.setProperty("os.arch", arch);
        check(NativeLibraryLoader.platform().equals("linux-x86_64"), "Linux x86_64 alias rejected: " + arch);
      }
    } finally {
      restore("os.name", originalOs);
      restore("os.arch", originalArch);
      restore("ame.native.path", originalNativePath);
      Files.delete(explicitLibrary);
      Files.delete(temporary);
    }
    LOG.info("AME Linux platform checks passed: 14 rejection cases and 3 x86_64 aliases");
  }

  private static void expectPlatformRejection(Path memory, String[] target) {
    try (AmeEngine ignored = AmeEngine.open(memory)) {
      throw new AssertionError("Unsupported platform was accepted: " + String.join("/", target));
    } catch (UnsatisfiedLinkError expected) {
      String message = expected.getMessage();
      check(message.contains("supports only Linux x86_64"), "Expected platform rejection, got: " + message);
      check(message.contains("os.name=" + target[0].toLowerCase(Locale.ROOT))
          && message.contains("os.arch=" + target[1]), "Rejection must identify the unsupported target");
    }
  }

  private static void restore(String property, String value) {
    if (value == null) System.clearProperty(property);
    else System.setProperty(property, value);
  }

  private static void check(boolean condition, String message) {
    if (!condition) throw new AssertionError(message);
  }
}
