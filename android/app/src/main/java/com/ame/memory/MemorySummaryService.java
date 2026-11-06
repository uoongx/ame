package com.ame.memory;

import android.util.Log;

import org.json.JSONArray;
import org.json.JSONException;
import org.json.JSONObject;

import java.util.ArrayList;
import java.util.LinkedHashSet;
import java.util.List;
import java.util.LinkedHashMap;

public final class MemorySummaryService {

    private static final String TAG = "AmeMem";

    private static final int MAX_KEYWORDS = 6;
    private static final int MAX_KEYWORD_CHARS = 40;
    private static final int MAX_CONTEXT_CHARS = 1500;
    public static final String MARK = "MEMSUM:";

    public static final class Summary {
        public String l1 = "";
        public String l2 = "";
        public List<String> keywords = new ArrayList<>();
        public String l3 = "";

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
            Summary s = new Summary();
            try {
                JSONObject jo = new JSONObject(json);
                s.l1 = prefix(jo.getString("l1").trim(), 60);
                if (s.l1.isEmpty()) throw new IllegalArgumentException("Empty summary");
                s.l2 = prefix(jo.optString("l2").trim(), 300);
                s.l3 = jo.optString("l3");
                JSONArray ka = jo.optJSONArray("keywords");
                if (ka != null) {
                    for (int i = 0; i < Math.min(ka.length(), MAX_KEYWORDS); i++) {
                        String term = prefix(ka.optString(i).trim(), MAX_KEYWORD_CHARS);
                        if (!term.isEmpty() && !s.keywords.contains(term)) s.keywords.add(term);
                    }
                }
            } catch (JSONException e) {
                throw new IllegalArgumentException("Invalid summary JSON", e);
            }
            return s;
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
        Summary s = new Summary();
        s.l3 = source;
        try {
            List<MiniMaxClient.Msg> msgs = new ArrayList<>();
            msgs.add(new MiniMaxClient.Msg("system", system));
            msgs.add(new MiniMaxClient.Msg("user", user));
            String resp = MiniMaxClient.chat(msgs, 4096);

            String text = resp.trim();
            if (text.startsWith("```")) {
                int nl = text.indexOf('\n');
                if (nl >= 0) text = text.substring(nl + 1);
            }
            int a = text.indexOf('{'), b = text.lastIndexOf('}');
            if (a < 0 || b <= a) throw new IllegalArgumentException("Missing summary object");
            Summary parsed = Summary.fromJson(text.substring(a, b + 1));
            parsed.l3 = source;
            return parsed;
        } catch (RuntimeException e) {
            Log.w(TAG, "Summary generation failed; using a source excerpt");
            s.l1 = prefix(source, 60);
            s.l2 = prefix(source, 300);
            return s;
        }
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
        } catch (IllegalArgumentException e) {
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
        for (Summary summary : fetchByKeywords(keywords, 3)) unique.putIfAbsent(summary.toJson(), summary);
        return buildContext(new ArrayList<>(unique.values()), 2);
    }

    private static String prefix(String text, int maxChars) {
        if (text.length() <= maxChars) return text;
        int end = Character.isHighSurrogate(text.charAt(maxChars - 1)) ? maxChars - 1 : maxChars;
        return text.substring(0, end);
    }
}
