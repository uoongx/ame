package com.ame.memory;

import java.util.HashSet;
import java.util.Locale;
import java.util.Set;

public final class FollowupGate {
    private static final int MAX_MARKED_FOLLOWUP_LENGTH = 30;
    private static final int MAX_RELATED_FOLLOWUP_LENGTH = 40;
    private static final int MIN_OVERLAP = 2;
    private static final int NEW_TOPIC_LENGTH_THRESHOLD = 12;
    private static final int MIN_ASCII_TOKEN_LENGTH = 3;
    private static final int MAX_INITIAL_TOKEN_CAPACITY = 256;
    private static final String[] FOLLOWUP_MARKERS = {
        "那", "还有", "继续", "详细", "具体", "为什么", "怎么办", "呢", "吗", "再"
    };

    public enum Stage { SIMPLE, DEEP }

    private Stage stage = Stage.SIMPLE;
    private String lastQuery = "";

    public Stage stage() {
        return stage;
    }

    public Stage onTurn(String query) {
        String current = query == null ? "" : query.trim();
        stage = nextStage(current, lastQuery, stage);
        lastQuery = current;
        return stage;
    }

    private static Stage nextStage(String query, String previous, Stage current) {
        if (previous.isEmpty() || query.isEmpty()) return current;
        if (explicitNewTopic(query)) return Stage.SIMPLE;
        if (markedFollowup(query)) return Stage.DEEP;
        int common = overlap(query, previous);
        if (query.length() <= MAX_RELATED_FOLLOWUP_LENGTH && common >= MIN_OVERLAP) return Stage.DEEP;
        if (query.length() > NEW_TOPIC_LENGTH_THRESHOLD && common == 0
                && !query.contains("呢") && !query.contains("吗")) return Stage.SIMPLE;
        return current;
    }

    public static boolean isFollowup(String query, String previous) {
        if (previous == null || query == null || query.length() > MAX_RELATED_FOLLOWUP_LENGTH) return false;
        return nextStage(query, previous, Stage.SIMPLE) == Stage.DEEP;
    }

    private static boolean explicitNewTopic(String query) {
        return query.contains("换个话题") || query.contains("新话题");
    }

    private static boolean markedFollowup(String query) {
        if (query.length() > MAX_MARKED_FOLLOWUP_LENGTH) return false;
        for (String marker : FOLLOWUP_MARKERS) {
            if (query.contains(marker)) return true;
        }
        return false;
    }

    static int overlap(String a, String b) {
        Set<String> first = tokens(a);
        Set<String> second = tokens(b);
        int count = 0;
        for (String term : first) {
            if (second.contains(term)) count++;
        }
        return count;
    }

    private static Set<String> tokens(String text) {
        Set<String> out = new HashSet<>(Math.min(text.length(), MAX_INITIAL_TOKEN_CAPACITY));
        StringBuilder ascii = new StringBuilder();
        char previousCjk = 0;
        for (int i = 0; i < text.length(); i++) {
            char c = text.charAt(i);
            if (c <= '\u007f' && Character.isLetterOrDigit(c)) {
                ascii.append(c);
                continue;
            }
            flushAscii(ascii, out);
            if (Character.UnicodeBlock.of(c) == Character.UnicodeBlock.CJK_UNIFIED_IDEOGRAPHS) {
                if (previousCjk != 0) out.add(String.valueOf(previousCjk) + c);
                previousCjk = c;
            } else {
                previousCjk = 0;
            }
        }
        flushAscii(ascii, out);
        return out;
    }

    private static void flushAscii(StringBuilder token, Set<String> out) {
        if (token.length() >= MIN_ASCII_TOKEN_LENGTH) out.add(token.toString().toLowerCase(Locale.ROOT));
        token.setLength(0);
    }
}
