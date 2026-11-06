package com.ame.memory;

import java.util.ArrayList;
import java.util.List;
import org.json.JSONArray;
import org.json.JSONObject;

public final class SummaryFlowTest {
    private static void check(boolean ok, String label) {
        if (!ok) throw new AssertionError(label);
    }

    public static void main(String[] args) {
        AmeEngine engine = new AmeEngine();
        MemoryOrchestrator chat = new MemoryOrchestrator(engine, new SessionStore(), "test");
        chat.chat("First museum visit", null, null, false, false);
        check("user".equals(engine.events.get(0).getString("speaker")), "legacy write");
        check(MiniMaxClient.generations == 0, "legacy model budget");

        BuildConfig.SUMMARY_MEMORY = true;
        chat.chat("The museum opens when?", null, null, false, false);
        check(MiniMaxClient.generations == 1 && engine.events.size() == 2, "one summary per turn");
        MemorySummaryService.Summary summary = MemorySummaryService.read(engine.events.get(1));
        check(summary.getL3().contains("The museum opens when?") && summary.getL3().contains("unique reply"),
                "original question and answer retained");

        chat.chat("新话题", null, null, false, false);
        check(MiniMaxClient.prompt.contains("Brief museum fact"), "first question uses L1");
        check(!MiniMaxClient.prompt.contains("Detailed hours"), "L2 deferred");
        check(!MiniMaxClient.prompt.contains("MEMSUM:"), "encoded event excluded from prompt");
        check(MiniMaxClient.prompt.contains("First museum visit"), "legacy events retained");

        chat.chat("更详细呢", null, null, false, false);
        check(MiniMaxClient.prompt.contains("Detailed hours") && engine.keywordReads > 0,
                "followup expands keywords and uses L2");

        MiniMaxClient.invalidJson = true;
        MemorySummaryService service = new MemorySummaryService(engine, "test");
        String source = "a".repeat(59) + "🎨" + "b".repeat(310);
        summary = service.summarize(source, "");
        check(summary.getL1().length() == 59 && summary.getL2().length() == 300 && source.equals(summary.getL3()),
                "malformed response fallback respects unicode and retains source");
        checkFailureBoundaries(service);
        checkImmutableSummary();
        checkStages();
        JSONObject fake = new JSONObject().put("text", "MEMSUM:{}").put("speaker", "user");
        check(!MemorySummaryService.isSummary(fake), "ordinary text cannot mark itself as a summary");
        check(service.buildContext(List.of(summary), 0).isEmpty(), "disabled context");
        check(service.buildContext(List.of(summary), 3).contains(source), "explicit L3");
        System.out.println("PASS Android summary write, recall and followup flow");
    }
    private static void checkFailureBoundaries(MemorySummaryService service) {
        MiniMaxClient.invalidJson = false;
        MiniMaxClient.failure = new MiniMaxClient.ChatResponseException("HTTP 503");
        check(service.summarize("a source", "").getL1().equals("a source"), "service failure fallback");
        MiniMaxClient.failure = new IllegalStateException("caller error");
        try {
            service.summarize("a source", "");
            throw new AssertionError("unexpected failure was swallowed");
        } catch (IllegalStateException expected) {
            check(expected == MiniMaxClient.failure, "original error must reach the caller");
        } finally {
            MiniMaxClient.failure = null;
        }
    }

    private static void checkImmutableSummary() {
        List<String> words = new ArrayList<>(List.of("x", "x", "y"));
        var summary = new MemorySummaryService.Summary("brief", "details", words, "source");
        words.clear();
        check(summary.getKeywords().equals(List.of("x", "y")), "keywords are privately owned");
        try {
            summary.getKeywords().clear();
            throw new AssertionError("summary exposed writable keywords");
        } catch (UnsupportedOperationException expected) {
            check(summary.getKeywords().size() == 2, "immutable keyword getter");
        }
        var restored = MemorySummaryService.Summary.fromJson(summary.toJson());
        check(restored.getKeywords().equals(summary.getKeywords()), "keywords survive round trip");
    }

    private static void checkStages() {
        FollowupGate gate = new FollowupGate();
        check(gate.onTurn("museum opening hours") == FollowupGate.Stage.SIMPLE, "initial stage");
        check(gate.onTurn("MUSEUM opening time") == FollowupGate.Stage.DEEP, "ASCII token overlap");
        check(gate.onTurn("嗯") == FollowupGate.Stage.DEEP, "ambiguous short turn preserves stage");
        check(gate.onTurn("换个话题，还有呢") == FollowupGate.Stage.SIMPLE, "new topic overrides markers");
        check(FollowupGate.isFollowup("北京故宫门票", "北京故宫"), "CJK overlap");
    }

}

final class BuildConfig {
    static final int CONTEXT_WINDOW_TURNS = 0, RECALL_TOP_K = 5;
    static boolean SUMMARY_MEMORY;
}

final class AmeEngine {
    final List<JSONObject> events = new ArrayList<>();
    int keywordReads;

    int rememberTurn(String speaker, String text, long ts, String id, String session) {
        events.add(new JSONObject().put("speaker", speaker).put("text", text).put("ts", ts).put("score", 1));
        return 0;
    }
    String recall(String query, int limit) {
        if ("museum".equals(query)) keywordReads++;
        return new JSONArray(events.subList(0, Math.min(events.size(), limit))).toString();
    }
    String getProfile() { return "{}"; }
    String stats() { return "{}"; }
    int save() { return 0; }
}

final class Message {
    final String role, text;
    Message(String role, String text, long ts, String image) { this.role = role; this.text = text; }
}

final class SessionStore {
    List<Message> loadMessages(String session) { return new ArrayList<>(); }
    void appendMessage(String session, Message message) {}
}

final class MiniMaxClient {
    static final class ChatResponseException extends RuntimeException {
        private static final long serialVersionUID = 1L;
        ChatResponseException(String message) { super(message); }
    }
    static RuntimeException failure;
    static final class Msg {
        final String role, text, imageB64;
        Msg(String role, String text) { this(role, text, null); }
        Msg(String role, String text, String image) { this.role = role; this.text = text; this.imageB64 = image; }
    }
    static int generations;
    static boolean invalidJson;
    static String prompt;
    static String chat(List<Msg> messages) { prompt = messages.get(0).text; return "unique reply"; }
    static String chat(List<Msg> messages, int tokens) {
        generations++;
        if (failure != null) throw failure;
        if (invalidJson) return "invalid summary";
        return new JSONObject().put("l1", "Brief museum fact").put("l2", "Detailed hours")
                .put("keywords", new JSONArray().put("museum")).toString();
    }
    static String visionCaption(String image) { throw new AssertionError("unexpected vision"); }
    static String visionForSearch(String image) { throw new AssertionError("unexpected vision"); }
    static String webSearch(String query) { throw new AssertionError("unexpected search"); }
    static List<Integer> rerank(String query, List<String> texts, int top) {
        throw new AssertionError("unexpected rerank");
    }
}
