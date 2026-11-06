package com.huawei.convai.ame;

import java.io.IOException;
import java.io.UncheckedIOException;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.Objects;

/**
 * An embedded AME engine backed by one directory. The caller owns the engine's lifecycle and
 * must not open the same directory concurrently in multiple instances or processes. The native
 * engine holds an exclusive directory lock and rejects a second open while that lock is held.
 *
 * <p>Access to an engine's native state is serialized. Results are JSON strings so applications may use
 * their own JSON library. This API does not call a model or accept model credentials.
 */
public final class AmeEngine implements AutoCloseable {
  private static final int JNI_VERSION = 2;
  private volatile long handle;

  private AmeEngine(long handle) {
    this.handle = handle;
  }

  /** Opens an existing snapshot, or creates an empty engine in the supplied directory. */
  public static AmeEngine open(Path directory) {
    Objects.requireNonNull(directory, "directory");
    Path absolute = directory.toAbsolutePath().normalize();
    NativeLibraryLoader.load();
    int version;
    try {
      version = nativeVersion();
    } catch (UnsatisfiedLinkError cause) {
      UnsatisfiedLinkError error = new UnsatisfiedLinkError(
          "The loaded library does not export the AME Java JNI ABI version entry point. "
              + "Use the desktop JNI library distributed with this SDK.");
      error.initCause(cause);
      throw error;
    }
    if (version != JNI_VERSION) {
      throw new UnsatisfiedLinkError(
          "AME JNI ABI mismatch: Java requires " + JNI_VERSION + ", native library reports "
              + version + ". Use the native library distributed with this SDK.");
    }
    try {
      Files.createDirectories(absolute);
      long opened = nativeOpen(absolute.toString());
      if (opened == 0) throw new IllegalStateException("AME nativeOpen returned an invalid handle");
      return new AmeEngine(opened);
    } catch (IOException error) {
      throw new UncheckedIOException("Unable to open AME directory: " + absolute, error);
    }
  }

  /**
   * Stores a conversation turn and returns its JSON write result. Use empty strings for optional
   * message/session identifiers; text and all string arguments must be non-null.
   */
  public synchronized String rememberTurn(
      String speaker, String text, long epochSeconds, String messageId, String sessionId) {
    long current = requireOpen();
    Objects.requireNonNull(speaker, "speaker");
    Objects.requireNonNull(text, "text");
    Objects.requireNonNull(messageId, "messageId");
    Objects.requireNonNull(sessionId, "sessionId");
    return nativeRememberTurn(current, speaker, text, epochSeconds, messageId, sessionId);
  }

  /** Returns a JSON array of matching events; limit must be between 1 and 100 inclusive. */
  public synchronized String recall(String query, int limit) {
    return recallFrom(query, limit, 0);
  }

  /** Combines private and tenant evidence for this query. Both engines must be open. */
  public String recallWithTenant(String query, int limit, AmeEngine tenant) {
    Objects.requireNonNull(tenant, "tenant");
    if (tenant == this) throw new IllegalArgumentException("Tenant must be a different engine");
    return recallFrom(query, limit, tenant.requireOpen());
  }

  private String recallFrom(String query, int limit, long tenantHandle) {
    long current = requireOpen();
    Objects.requireNonNull(query, "query");
    if (query.isBlank()) throw new IllegalArgumentException("query must not be blank");
    if (limit < 1 || limit > 100) throw new IllegalArgumentException("limit must be between 1 and 100");
    return nativeRecall(current, query, limit, tenantHandle);
  }

  /** Hides an event from recall. Returns false for an unknown event ID. */
  public synchronized boolean blockEvent(String eventId) {
    return nativeBlockEvent(requireOpen(), Objects.requireNonNull(eventId, "eventId"));
  }

  /** Makes a blocked event eligible for recall again. */
  public synchronized boolean unblockEvent(String eventId) {
    return nativeUnblockEvent(requireOpen(), Objects.requireNonNull(eventId, "eventId"));
  }

  /** Deletes the event, incident edges, and profile slots sourced from it. */
  public synchronized boolean deleteEvent(String eventId) {
    return nativeDeleteEvent(requireOpen(), Objects.requireNonNull(eventId, "eventId"));
  }

  /** Clears the entire open memory store. Persist the change with save() or close(). */
  public synchronized void clearMemory() {
    nativeClearMemory(requireOpen());
  }

  /** Returns a JSON object containing engine statistics. */
  public synchronized String stats() {
    return nativeStats(requireOpen());
  }

  /** Saves the current state. I/O failures are reported as {@link UncheckedIOException}. */
  public synchronized void save() {
    long current = requireOpen();
    try {
      nativeSave(current);
    } catch (IOException error) {
      throw new UncheckedIOException("Unable to save AME state", error);
    }
  }

  /**
   * Saves and releases the native engine. Repeated calls do nothing. Even if saving fails, the
   * native handle is released and this object remains closed.
   */
  @Override
  public synchronized void close() {
    if (handle == 0) return;
    long current = handle;
    handle = 0;
    try {
      nativeClose(current);
    } catch (IOException error) {
      throw new UncheckedIOException("Unable to save AME state while closing", error);
    }
  }

  private long requireOpen() {
    long current = handle;
    if (current == 0) throw new IllegalStateException("AME engine is closed");
    return current;
  }

  private static native long nativeOpen(String directory) throws IOException;

  private static native String nativeRememberTurn(
      long handle, String speaker, String text, long epochSeconds, String messageId, String sessionId);

  private static native String nativeRecall(long handle, String query, int limit, long tenantHandle);

  private static native boolean nativeBlockEvent(long handle, String eventId);
  private static native boolean nativeUnblockEvent(long handle, String eventId);
  private static native boolean nativeDeleteEvent(long handle, String eventId);
  private static native void nativeClearMemory(long handle);

  private static native String nativeStats(long handle);

  private static native void nativeSave(long handle) throws IOException;

  private static native void nativeClose(long handle) throws IOException;

  private static native int nativeVersion();
}
