package com.ame.memory;

import java.io.File;
import java.io.IOException;
import java.io.UncheckedIOException;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.Collections;
import java.util.Comparator;
import java.util.List;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.RejectedExecutionException;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicReference;
import java.util.logging.Logger;
import java.util.stream.Stream;

/** Pure Java lifecycle regression: no Android runtime, JNI, model or network calls. */
public final class ConversationWorkQueueTest {
    private static final Logger LOG = Logger.getLogger(ConversationWorkQueueTest.class.getName());
    private static final int TIMEOUT_SECONDS = 5;

    private ConversationWorkQueueTest() {}

    public static void main(String[] args) throws Exception {
        try (TestDirectory files = new TestDirectory()) {
            reopenReadsCompletedSnapshot(files.session("reopen"));
            twoLeasesRetainTheirSharedWorker(files.session("leases"));
            differentDirectoriesRunIndependently(files.session("independent-a"),
                    files.session("independent-b"));
            failedCleanupStillReleasesWorker(files.session("failed-cleanup"));
            repeatedCloseRunsCleanupOnce(files.session("double-close"));
            closedLeaseRejectsWork(files.session("closed"));
            canonicalAliasesShareWorker(files.session("canonical"));
        }
        LOG.info("ConversationWorkQueue: 7 lifecycle and persistence regressions passed");
    }

    private static void reopenReadsCompletedSnapshot(File directory) throws Exception {
        Path snapshot = directory.toPath().resolve("graph.json");
        Files.writeString(snapshot, "previous snapshot");
        ConversationWorkQueue previous = ConversationWorkQueue.acquire(directory);
        CountDownLatch started = new CountDownLatch(1);
        CountDownLatch release = new CountDownLatch(1);
        CountDownLatch loaded = new CountDownLatch(1);
        List<String> order = Collections.synchronizedList(new ArrayList<>());
        AtomicReference<String> observed = new AtomicReference<>();
        ConversationWorkQueue replacement = null;
        try {
            previous.execute(() -> {
                started.countDown();
                await(release);
                order.add("old request finished");
            });
            await(started);
            previous.close(() -> {
                write(snapshot, "completed snapshot");
                order.add("old snapshot saved");
            });
            replacement = ConversationWorkQueue.acquire(directory);
            replacement.execute(() -> {
                observed.set(read(snapshot));
                order.add("new snapshot loaded");
                loaded.countDown();
            });
            check(!loaded.await(100, TimeUnit.MILLISECONDS), "New initialization overtook old work");
            release.countDown();
            await(loaded);
            check("completed snapshot".equals(observed.get()), "Reopened engine read stale persistence");
            check(order.equals(List.of("old request finished", "old snapshot saved", "new snapshot loaded")),
                    "Close/reopen ordering changed: " + order);
            closeAndAwait(replacement);
        } finally {
            release.countDown();
            previous.close(() -> {});
            if (replacement != null) replacement.close(() -> {});
        }
    }

    private static void twoLeasesRetainTheirSharedWorker(File directory) throws Exception {
        ConversationWorkQueue first = ConversationWorkQueue.acquire(directory);
        ConversationWorkQueue second = ConversationWorkQueue.acquire(directory);
        AtomicReference<Thread> worker = new AtomicReference<>();
        CountDownLatch firstClosed = new CountDownLatch(1);
        CountDownLatch secondRan = new CountDownLatch(1);
        try {
            first.close(() -> {
                worker.set(Thread.currentThread());
                firstClosed.countDown();
            });
            await(firstClosed);
            second.execute(() -> {
                check(Thread.currentThread() == worker.get(), "Active lease lost its shared worker");
                secondRan.countDown();
            });
            await(secondRan);
            closeAndAwait(second);
            check(!worker.get().isAlive(), "Final lease did not terminate the worker");
        } finally {
            first.close(() -> {});
            second.close(() -> {});
        }
    }

    private static void differentDirectoriesRunIndependently(File firstDirectory, File secondDirectory)
            throws Exception {
        ConversationWorkQueue first = ConversationWorkQueue.acquire(firstDirectory);
        ConversationWorkQueue second = ConversationWorkQueue.acquire(secondDirectory);
        CountDownLatch started = new CountDownLatch(1);
        CountDownLatch release = new CountDownLatch(1);
        CountDownLatch secondRan = new CountDownLatch(1);
        try {
            first.execute(() -> {
                started.countDown();
                await(release);
            });
            await(started);
            second.execute(secondRan::countDown);
            await(secondRan);
            closeAndAwait(second);
            release.countDown();
            closeAndAwait(first);
        } finally {
            release.countDown();
            first.close(() -> {});
            second.close(() -> {});
        }
    }

    private static void failedCleanupStillReleasesWorker(File directory) throws Exception {
        ConversationWorkQueue queue = ConversationWorkQueue.acquire(directory);
        AtomicReference<Thread> originalWorker = new AtomicReference<>();
        AtomicReference<Throwable> failure = new AtomicReference<>();
        CountDownLatch failed = new CountDownLatch(1);
        queue.execute(() -> {
            originalWorker.set(Thread.currentThread());
            Thread.currentThread().setUncaughtExceptionHandler((thread, error) -> {
                failure.set(error);
                failed.countDown();
            });
        });
        queue.close(() -> { throw new IllegalStateException("expected cleanup failure"); });
        await(failed);
        awaitStopped(originalWorker.get());
        check(failure.get() instanceof IllegalStateException, "Cleanup failure was swallowed or replaced");
        ConversationWorkQueue reopened = ConversationWorkQueue.acquire(directory);
        try {
            AtomicReference<Thread> replacement = new AtomicReference<>();
            CountDownLatch ran = new CountDownLatch(1);
            reopened.execute(() -> {
                replacement.set(Thread.currentThread());
                ran.countDown();
            });
            await(ran);
            check(replacement.get() != originalWorker.get(), "Failed cleanup left a stale registry entry");
            closeAndAwait(reopened);
        } finally {
            reopened.close(() -> {});
        }
    }

    private static void repeatedCloseRunsCleanupOnce(File directory) throws Exception {
        ConversationWorkQueue queue = ConversationWorkQueue.acquire(directory);
        AtomicInteger cleanupCalls = new AtomicInteger();
        AtomicReference<Thread> worker = new AtomicReference<>();
        CountDownLatch cleaned = new CountDownLatch(1);
        queue.close(() -> {
            worker.set(Thread.currentThread());
            cleanupCalls.incrementAndGet();
            cleaned.countDown();
        });
        queue.close(cleanupCalls::incrementAndGet);
        await(cleaned);
        awaitStopped(worker.get());
        check(cleanupCalls.get() == 1, "Repeated close ran cleanup more than once");
    }

    private static void closedLeaseRejectsWork(File directory) throws Exception {
        ConversationWorkQueue queue = ConversationWorkQueue.acquire(directory);
        closeAndAwait(queue);
        try {
            queue.execute(() -> { throw new AssertionError("Closed lease executed new work"); });
            throw new AssertionError("Closed lease accepted new work");
        } catch (RejectedExecutionException expected) {
            check(expected.getMessage().contains("closed"), "Closed-lease rejection is unclear");
        }
    }

    private static void canonicalAliasesShareWorker(File directory) throws Exception {
        Files.createDirectory(directory.toPath().resolve("child"));
        File alias = new File(directory, "child/..");
        ConversationWorkQueue first = ConversationWorkQueue.acquire(directory);
        ConversationWorkQueue second = ConversationWorkQueue.acquire(alias);
        CountDownLatch started = new CountDownLatch(1);
        CountDownLatch release = new CountDownLatch(1);
        CountDownLatch secondRan = new CountDownLatch(1);
        AtomicReference<Thread> firstWorker = new AtomicReference<>();
        AtomicReference<Thread> secondWorker = new AtomicReference<>();
        try {
            check(first.directory().equals(directory.getCanonicalPath()), "Directory is not canonical");
            check(first.directory().equals(second.directory()), "Aliases use different directory keys");
            first.execute(() -> {
                firstWorker.set(Thread.currentThread());
                started.countDown();
                await(release);
            });
            await(started);
            second.execute(() -> {
                secondWorker.set(Thread.currentThread());
                secondRan.countDown();
            });
            check(!secondRan.await(100, TimeUnit.MILLISECONDS), "Alias bypassed the session queue");
            release.countDown();
            await(secondRan);
            check(firstWorker.get() == secondWorker.get(), "Aliases did not reuse the worker");
            first.close(() -> {});
            closeAndAwait(second);
        } finally {
            release.countDown();
            first.close(() -> {});
            second.close(() -> {});
        }
    }

    private static void closeAndAwait(ConversationWorkQueue queue) throws InterruptedException {
        CountDownLatch cleaned = new CountDownLatch(1);
        AtomicReference<Thread> worker = new AtomicReference<>();
        queue.close(() -> {
            worker.set(Thread.currentThread());
            cleaned.countDown();
        });
        await(cleaned);
        awaitStopped(worker.get());
    }

    private static void awaitStopped(Thread thread) throws InterruptedException {
        thread.join(TimeUnit.SECONDS.toMillis(TIMEOUT_SECONDS));
        check(!thread.isAlive(), "Unreferenced session worker was not shut down");
    }

    private static void await(CountDownLatch latch) {
        try {
            check(latch.await(TIMEOUT_SECONDS, TimeUnit.SECONDS), "Timed out waiting for queue work");
        } catch (InterruptedException error) {
            Thread.currentThread().interrupt();
            throw new AssertionError("Interrupted waiting for queue work", error);
        }
    }

    private static void write(Path file, String text) {
        try {
            Files.writeString(file, text);
        } catch (IOException error) {
            throw new UncheckedIOException(error);
        }
    }

    private static String read(Path file) {
        try {
            return Files.readString(file);
        } catch (IOException error) {
            throw new UncheckedIOException(error);
        }
    }

    private static void check(boolean condition, String message) {
        if (!condition) throw new AssertionError(message);
    }

    private static final class TestDirectory implements AutoCloseable {
        private final Path root = Files.createTempDirectory("ame-conversation-queue-");

        TestDirectory() throws IOException {}

        File session(String name) throws IOException {
            return Files.createDirectory(root.resolve(name)).toFile();
        }

        @Override
        public void close() throws IOException {
            try (Stream<Path> files = Files.walk(root)) {
                files.sorted(Comparator.reverseOrder()).forEach(file -> {
                    try {
                        Files.delete(file);
                    } catch (IOException error) {
                        throw new UncheckedIOException(error);
                    }
                });
            }
        }
    }
}
