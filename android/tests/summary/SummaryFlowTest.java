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
        check(summary.l3.contains("The museum opens when?") && summary.l3.contains("unique reply"),
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
        check(summary.l1.length() == 59 && summary.l2.length() == 300 && source.equals(summary.l3),
                "malformed response fallback respects unicode and retains source");
        JSONObject fake = new JSONObject().put("text", "MEMSUM:{}").put("speaker", "user");
        check(!MemorySummaryService.isSummary(fake), "ordinary text cannot mark itself as a summary");
        check(service.buildContext(List.of(summary), 0).isEmpty(), "disabled context");
        check(service.buildContext(List.of(summary), 3).contains(source), "explicit L3");
        System.out.println("PASS Android summary write, recall and followup flow");
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
