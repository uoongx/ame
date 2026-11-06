package com.ame.memory;

import java.util.HashSet;
import java.util.Set;
import java.util.Locale;

public final class FollowupGate {

    public enum Stage { SIMPLE, DEEP }

    private Stage stage = Stage.SIMPLE;
    private String lastQuery = "";

    public Stage stage() {
        return stage;
    }

    public Stage onTurn(String query) {
        String q = query == null ? "" : query.trim();
        if (isFollowup(q, lastQuery)) {
            stage = Stage.DEEP;
        } else if (isNewTopic(q, lastQuery)) {
            stage = Stage.SIMPLE;
        }
        lastQuery = q;
        return stage;
    }

    public static boolean isFollowup(String q, String prev) {
        if (prev == null || prev.isEmpty() || q.isEmpty()) return false;
        if (q.contains("换个话题") || q.contains("新话题")) return false;
        if (q.length() <= 30) {
            String[] marks = {"那", "还有", "继续", "详细", "具体", "为什么", "怎么办", "呢", "吗", "再"};
            for (String m : marks)
                if (q.contains(m)) return true;
        }
        return q.length() <= 40 && overlap(q, prev) >= 2;
    }

    private static boolean isNewTopic(String q, String prev) {
        if (prev == null || prev.isEmpty() || q.isEmpty()) return false;
        if (q.contains("换个话题") || q.contains("新话题")) return true;
        return q.length() > 12 && overlap(q, prev) == 0 && !q.contains("呢") && !q.contains("吗");
    }

    static int overlap(String a, String b) {
        Set<String> ta = tokens(a), tb = tokens(b);
        int n = 0;
        for (String t : ta)
            if (tb.contains(t)) n++;
        return n;
    }

    private static Set<String> tokens(String s) {
        Set<String> out = new HashSet<>();
        StringBuilder ascii = new StringBuilder();
        StringBuilder cjk = new StringBuilder();
        Runnable flushAscii = () -> {
            if (ascii.length() >= 3) out.add(ascii.toString().toLowerCase(Locale.ROOT));
            ascii.setLength(0);
        };
        for (int i = 0; i < s.length(); i++) {
            char c = s.charAt(i);
            if (c < 128 && Character.isLetterOrDigit(c)) {
                ascii.append(c);
            } else {
                flushAscii.run();
                if (c >= 0x4E00 && c <= 0x9FFF) {
                    cjk.append(c);
                    if (cjk.length() >= 2) {
                        out.add(cjk.substring(cjk.length() - 2));
                        cjk.deleteCharAt(0);
                    }
                } else {
                    cjk.setLength(0);
                }
            }
        }
        flushAscii.run();
        return out;
    }
}
