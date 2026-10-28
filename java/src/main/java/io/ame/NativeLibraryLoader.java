package io.ame;

import java.io.IOException;
import java.io.InputStream;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.StandardCopyOption;
import java.util.Locale;

/** Loads a Linux x86_64 native library from the JAR or an explicitly supplied path. */
final class NativeLibraryLoader {
  private static boolean loaded;

  private NativeLibraryLoader() {}

  static synchronized void load() {
    String platform = platform();
    if (loaded) return;
    String configured = System.getProperty("ame.native.path");
    if (configured != null) {
      if (configured.isBlank()) {
        throw failure("ame.native.path must name a native library file, not an empty path", null);
      }
      Path library = Path.of(configured).toAbsolutePath().normalize();
      if (!Files.isRegularFile(library)) {
        throw failure("ame.native.path is not a native library file: " + library, null);
      }
      loadFile(library, "ame.native.path");
      loaded = true;
      return;
    }

    String fileName = "libame_jni.so";
    String resource = "/io/ame/native/" + platform + "/" + fileName;
    try (InputStream source = NativeLibraryLoader.class.getResourceAsStream(resource)) {
      if (source == null) {
        throw failure(
            "This AME JAR does not contain a native library for " + platform + " (expected "
                + resource + "). Package the matching native library or set -Dame.native.path="
                + "<absolute-library-path>.", null);
      }
      Path directory = Files.createTempDirectory("ame-jni-");
      Path extracted = directory.resolve(fileName);
      // Register the directory first: shutdown cleanup runs in reverse registration order.
      directory.toFile().deleteOnExit();
      extracted.toFile().deleteOnExit();
      Files.copy(source, extracted, StandardCopyOption.REPLACE_EXISTING);
      loadFile(extracted, "JAR resource " + resource);
      loaded = true;
    } catch (IOException error) {
      throw failure("Unable to extract AME native library " + resource + " to java.io.tmpdir", error);
    }
  }

  private static void loadFile(Path library, String origin) {
    try {
      System.load(library.toString());
    } catch (UnsatisfiedLinkError | SecurityException error) {
      throw failure(
          "Unable to load AME native library from " + origin + " at " + library
              + ". Check its OS/CPU architecture, native dependencies and load permissions. "
              + error.getMessage(), error);
    }
  }

  static String platform() {
    String os = System.getProperty("os.name", "").toLowerCase(Locale.ROOT);
    String arch = System.getProperty("os.arch", "").toLowerCase(Locale.ROOT);
    if (!os.equals("linux") || !(arch.equals("amd64") || arch.equals("x86_64") || arch.equals("x64"))) {
      throw failure("AME Java SDK supports only Linux x86_64; detected os.name=" + os
          + ", os.arch=" + arch, null);
    }
    return "linux-x86_64";
  }

  private static UnsatisfiedLinkError failure(String message, Throwable cause) {
    UnsatisfiedLinkError error = new UnsatisfiedLinkError(message);
    if (cause != null) error.initCause(cause);
    return error;
  }
}
