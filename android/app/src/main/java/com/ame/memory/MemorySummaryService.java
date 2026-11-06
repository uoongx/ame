package com.ame.memory;

import android.util.Log;

import org.json.JSONArray;
import org.json.JSONException;
import org.json.JSONObject;

import java.util.ArrayList;
import java.util.Collections;
import java.util.LinkedHashMap;
import java.util.LinkedHashSet;
import java.util.List;
import java.util.Objects;

public final class MemorySummaryService {

    private static final String TAG = "AmeMem";

    private static final int BRIEF_CHARS = 60;
    private static final int DETAIL_CHARS = 300;
    private static final int SUMMARY_TOKEN_BUDGET = 4096;
    private static final int KEYWORD_HIT_LIMIT = 3;
    private static final int MAX_KEYWORDS = 6;
    private static final int MAX_KEYWORD_CHARS = 40;
    private static final int MAX_CONTEXT_CHARS = 1500;
    public static final String MARK = "MEMSUM:";

    private static final class SummaryFormatException extends IllegalArgumentException {
        private static final long serialVersionUID = 1L;

        SummaryFormatException(String message) { super(message); }
        SummaryFormatException(JSONException cause) { super("Invalid summary JSON", cause); }
    }

    public static final class Summary {
        private final String l1;
        private final String l2;
        private final List<String> keywords;
        private final String l3;

        public Summary() {
            this("", "", Collections.emptyList(), "");
        }

        public Summary(String l1, String l2, List<String> keywords, String l3) {
            this.l1 = prefix(Objects.requireNonNull(l1, "l1"), BRIEF_CHARS);
            this.l2 = prefix(Objects.requireNonNull(l2, "l2"), DETAIL_CHARS);
            this.l3 = Objects.requireNonNull(l3, "l3");
            this.keywords = copyKeywords(Objects.requireNonNull(keywords, "keywords"));
        }

        public String getL1() { return l1; }
        public String getL2() { return l2; }
        public List<String> getKeywords() { return keywords; }
        public String getL3() { return l3; }

        private static List<String> copyKeywords(List<String> words) {
            int limit = Math.min(words.size(), MAX_KEYWORDS);
            List<String> terms = new ArrayList<>(limit);
            for (int i = 0; i < limit; i++) {
                String term = prefix(words.get(i).trim(), MAX_KEYWORD_CHARS);
                if (!term.isEmpty() && !terms.contains(term)) terms.add(term);
            }
            return Collections.unmodifiableList(terms);
        }

        public String toJson() {
            try {
                return new JSONObject()
                        .put("l1", l1)
                        .put("l2", l2)
                        .put("keywords", new JSONArray(keywords))
                        .put("l3", l3)
                        .toString();
            } catch (JSONException e) {
                throw new IllegalArgumentException("Invalid summary JSON", e);
            }
        }

        public static Summary fromJson(String json) {
            return parse(json, null);
        }

        private static Summary parse(String json, String source) {
            try {
                JSONObject jo = new JSONObject(json);
                String l1 = jo.getString("l1").trim();
                if (l1.isEmpty()) throw new SummaryFormatException("Empty summary");
                String original = source == null ? jo.optString("l3") : source;
                return new Summary(l1, jo.optString("l2").trim(), readKeywords(jo), original);
            } catch (JSONException e) {
                throw new SummaryFormatException(e);
            }
        }

        private static List<String> readKeywords(JSONObject jo) {
            JSONArray words = jo.optJSONArray("keywords");
            if (words == null) return Collections.emptyList();
            int limit = Math.min(words.length(), MAX_KEYWORDS);
            List<String> terms = new ArrayList<>(limit);
            for (int i = 0; i < limit; i++) terms.add(words.optString(i));
            return terms;
        }
    }

    private final AmeEngine engine;
    private final String sessionId;

    public MemorySummaryService(AmeEngine engine, String sessionId) {
        this.engine = engine;
        this.sessionId = sessionId;
    }

    public Summary summarize(String source, String question) {
        String system = "你是记忆整理器。把给定材料整理成严格 JSON（不要输出任何其他文字）："
                + "{\"l1\":\"不超过60字的基础摘要，言简意赅\","
                + "\"l2\":\"不超过300字的详细整理，面向难以理解的问题给出要点、因果和时序\","
                + "\"keywords\":[\"3-6个联想词，用于扩展检索相关知识，名词优先\"]}。"
                + "要求：l1 必须提炼关键事实（人物/时间/地点/数字/结论），不得只复述首句；"
                + "keywords 必填且不得为空数组。";
        String user = (question == null || question.isEmpty() ? "" : "当前问题：" + question + "\n")
                + "材料：\n" + source;
        Objects.requireNonNull(source, "source");
        List<MiniMaxClient.Msg> msgs = new ArrayList<>(2);
        msgs.add(new MiniMaxClient.Msg("system", system));
        msgs.add(new MiniMaxClient.Msg("user", user));
        try {
            String response = MiniMaxClient.chat(msgs, SUMMARY_TOKEN_BUDGET);
            return Summary.parse(summaryObject(response), source);
        } catch (MiniMaxClient.ChatResponseException | SummaryFormatException e) {
            Log.w(TAG, "Summary generation failed; using a source excerpt");
            return new Summary(source, source, Collections.emptyList(), source);
        }
    }

    private static String summaryObject(String response) {
        String text = response.trim();
        if (text.startsWith("```")) {
            int newline = text.indexOf('\n');
            if (newline >= 0) text = text.substring(newline + 1);
        }
        int start = text.indexOf('{');
        int end = text.lastIndexOf('}');
        if (start < 0 || end <= start) throw new SummaryFormatException("Missing summary object");
        return text.substring(start, end + 1);
    }

    public boolean storeToAme(Summary summary, long ts) {
        return engine.rememberTurn("summary", MARK + summary.toJson(), ts, "sum" + ts, sessionId) == 0;
    }

    int remember(String speaker, String source, long ts, String id, String question) {
        if (!BuildConfig.SUMMARY_MEMORY) return engine.rememberTurn(speaker, source, ts, id, sessionId);
        Summary summary = summarize(source, question);
        return engine.rememberTurn("summary", MARK + summary.toJson(), ts, id, sessionId);
    }

    static boolean isSummary(JSONObject event) {
        return "summary".equals(event.optString("speaker")) && event.optString("text").startsWith(MARK);
    }

    static Summary read(JSONObject event) {
        return Summary.fromJson(event.optString("text").substring(MARK.length()));
    }

    public List<Summary> fetchFromAme(String query, int limit) {
        List<Summary> out = new ArrayList<>();
        try {
            JSONArray events = new JSONArray(engine.recall(query, limit));
            for (int i = 0; i < events.length(); i++) {
                JSONObject event = events.optJSONObject(i);
                if (event == null || !isSummary(event)) continue;
                addIfValid(event, out);
            }
        } catch (JSONException e) {
            Log.w(TAG, "Invalid recall response");
        }
        return out;
    }

    static void addIfValid(JSONObject event, List<Summary> out) {
        try {
            out.add(read(event));
        } catch (SummaryFormatException e) {
            Log.w(TAG, "Ignoring an invalid stored summary");
        }
    }

    public List<Summary> fetchByKeywords(List<String> keywords, int limitPerKeyword) {
        List<Summary> out = new ArrayList<>();
        LinkedHashSet<String> terms = new LinkedHashSet<>();
        for (String word : keywords) {
            if (word != null && !word.trim().isEmpty()) terms.add(word.trim());
            if (terms.size() == MAX_KEYWORDS) break;
        }
        LinkedHashSet<String> seen = new LinkedHashSet<>();
        for (String term : terms) {
            for (Summary summary : fetchFromAme(term, limitPerKeyword)) {
                if (seen.add(summary.toJson())) out.add(summary);
            }
        }
        return out;
    }

    public String buildContext(List<Summary> items, int level) {
        if (items == null || level <= 0) return "";
        StringBuilder text = new StringBuilder();
        for (Summary item : items) {
            text.append("【摘要】").append(item.l1).append('\n');
            if (level >= 2 && !item.l2.isEmpty()) text.append("【详述】").append(item.l2).append('\n');
            if (level >= 2 && !item.keywords.isEmpty())
                text.append("【联想词】").append(String.join("、", item.keywords)).append('\n');
            if (level >= 3) text.append("【原文】").append(item.l3).append('\n');
            if (level <= 2 && text.length() >= MAX_CONTEXT_CHARS) break;
        }
        return level <= 2 ? prefix(text.toString(), MAX_CONTEXT_CHARS) : text.toString();
    }

    String contextFor(List<Summary> originals, FollowupGate.Stage stage) {
        if (stage == FollowupGate.Stage.SIMPLE) return buildContext(originals, 1);
        LinkedHashMap<String, Summary> unique = new LinkedHashMap<>();
        List<String> keywords = new ArrayList<>();
        for (Summary summary : originals) {
            unique.putIfAbsent(summary.toJson(), summary);
            keywords.addAll(summary.keywords);
        }
        for (Summary summary : fetchByKeywords(keywords, KEYWORD_HIT_LIMIT)) unique.putIfAbsent(summary.toJson(), summary);
        return buildContext(new ArrayList<>(unique.values()), 2);
    }

    private static String prefix(String text, int maxChars) {
        if (text.length() <= maxChars) return text;
        int end = Character.isHighSurrogate(text.charAt(maxChars - 1)) ? maxChars - 1 : maxChars;
        return text.substring(0, end);
    }
}
