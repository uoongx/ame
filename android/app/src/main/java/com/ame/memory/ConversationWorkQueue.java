package com.ame.memory;

import java.io.File;
import java.io.IOException;
import java.util.HashMap;
import java.util.Map;
import java.util.Objects;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.RejectedExecutionException;
import java.util.concurrent.atomic.AtomicInteger;

/** A lease on the serial work queue for one canonical session directory. */
final class ConversationWorkQueue {
    private static final Map<String, SharedQueue> QUEUES = new HashMap<>();
    private static final AtomicInteger THREAD_IDS = new AtomicInteger();

    private static final class SharedQueue {
        final String directory;
        final ExecutorService executor = Executors.newSingleThreadExecutor(task ->
                new Thread(task, "ame-conversation-" + THREAD_IDS.incrementAndGet()));
        int leases;

        SharedQueue(String directory) {
            this.directory = directory;
        }
    }

    private final SharedQueue queue;
    // Protected by QUEUES, together with submissions and the shared lease count.
    private boolean closed;

    private ConversationWorkQueue(SharedQueue queue) {
        this.queue = queue;
    }

    static ConversationWorkQueue acquire(File directory) throws IOException {
        String canonical = Objects.requireNonNull(directory, "directory").getCanonicalPath();
        synchronized (QUEUES) {
            SharedQueue queue = QUEUES.get(canonical);
            if (queue == null) {
                queue = new SharedQueue(canonical);
                QUEUES.put(canonical, queue);
            }
            ++queue.leases;
            return new ConversationWorkQueue(queue);
        }
    }

    String directory() {
        return queue.directory;
    }

    void execute(Runnable work) {
        Objects.requireNonNull(work, "work");
        synchronized (QUEUES) {
            if (closed) throw new RejectedExecutionException("Conversation queue lease is closed");
            queue.executor.execute(work);
        }
    }

    /**
     * Reject new work on this lease and enqueue its cleanup after earlier submissions.
     * The registry entry remains live until cleanup finishes, so a replacement Activity
     * joins this queue and cannot initialize before the previous cleanup completes.
     * Repeated calls do nothing. Cleanup failures follow the executor's uncaught-exception
     * handling; the lease is released even when cleanup fails.
     */
    void close(Runnable cleanup) {
        Objects.requireNonNull(cleanup, "cleanup");
        synchronized (QUEUES) {
            if (closed) return;
            closed = true;
            queue.executor.execute(() -> cleanUpAndRelease(cleanup));
        }
    }

    private void cleanUpAndRelease(Runnable cleanup) {
        try {
            cleanup.run();
        } finally {
            releaseQueue();
        }
    }

    private void releaseQueue() {
        synchronized (QUEUES) {
            if (--queue.leases == 0) {
                QUEUES.remove(queue.directory, queue);
                queue.executor.shutdown();
            }
        }
    }
}
