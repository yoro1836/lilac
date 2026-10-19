package com.hexadecinull.vineos.agent;

import java.io.ByteArrayOutputStream;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;
import java.util.regex.Pattern;

// Pure parsing and framing for the host link. No Android classes, so it runs and tests on any JVM.
public final class AgentProtocol {
    public static final int MAX_POINTERS = 10;
    public static final int MAX_LINE_BYTES = 256 * 1024;
    private static final Pattern PACKAGE = Pattern.compile("[A-Za-z][A-Za-z0-9_]*(\\.[A-Za-z0-9_]+)+");

    private AgentProtocol() {}

    public static final class Command {
        public final String name;
        public final String rest;

        Command(String name, String rest) {
            this.name = name;
            this.rest = rest;
        }

        public String[] words() {
            return rest.isEmpty() ? new String[0] : rest.split(" ");
        }
    }

    public static final class Touch {
        public final int action;
        public final int actionIndex;
        public final int[] ids;
        public final float[] x;
        public final float[] y;

        Touch(int action, int actionIndex, int[] ids, float[] x, float[] y) {
            this.action = action;
            this.actionIndex = actionIndex;
            this.ids = ids;
            this.x = x;
            this.y = y;
        }
    }

    // Splits "name rest of the line"; null for a blank line
    public static Command parse(String line) {
        if (line == null) return null;
        final String t = line.trim();
        if (t.isEmpty()) return null;
        final int sp = t.indexOf(' ');
        return sp < 0 ? new Command(t, "") : new Command(t.substring(0, sp), t.substring(sp + 1));
    }

    // touch <action> <actionIndex> <n> then n times <id> <x> <y>, with x and y as fractions of the screen
    public static Touch parseTouch(String rest) {
        final String[] w = rest.isEmpty() ? new String[0] : rest.split(" ");
        if (w.length < 3) return null;
        final Integer action = intOrNull(w[0]);
        final Integer index = intOrNull(w[1]);
        final Integer n = intOrNull(w[2]);
        if (action == null || index == null || n == null) return null;
        if (n < 1 || n > MAX_POINTERS || w.length != 3 + n * 3) return null;
        if (index < 0 || index >= n) return null;
        final int[] ids = new int[n];
        final float[] xs = new float[n];
        final float[] ys = new float[n];
        for (int i = 0; i < n; i++) {
            final Integer id = intOrNull(w[3 + i * 3]);
            final Float x = floatOrNull(w[4 + i * 3]);
            final Float y = floatOrNull(w[5 + i * 3]);
            if (id == null || x == null || y == null) return null;
            ids[i] = id;
            xs[i] = clamp01(x);
            ys[i] = clamp01(y);
        }
        return new Touch(action, index, ids, xs, ys);
    }

    // Maps host pointer ids to small stable slots, lowest free one first
    public static final class PointerTracker {
        private final int[] hostIds = new int[MAX_POINTERS];
        private final boolean[] used = new boolean[MAX_POINTERS];

        public int[] map(int[] ids) {
            final int[] out = new int[ids.length];
            for (int i = 0; i < ids.length; i++) out[i] = slotFor(ids[i]);
            return out;
        }

        public void release(int hostId) {
            for (int s = 0; s < MAX_POINTERS; s++) {
                if (used[s] && hostIds[s] == hostId) used[s] = false;
            }
        }

        public void clear() {
            Arrays.fill(used, false);
        }

        private int slotFor(int hostId) {
            for (int s = 0; s < MAX_POINTERS; s++) {
                if (used[s] && hostIds[s] == hostId) return s;
            }
            for (int s = 0; s < MAX_POINTERS; s++) {
                if (!used[s]) {
                    used[s] = true;
                    hostIds[s] = hostId;
                    return s;
                }
            }
            used[MAX_POINTERS - 1] = true; // all slots busy, reuse the last one
            hostIds[MAX_POINTERS - 1] = hostId;
            return MAX_POINTERS - 1;
        }
    }

    public static boolean validKeycode(int code) {
        return code > 0 && code < 1000;
    }

    public static boolean validPackage(String name) {
        return name != null && name.length() <= 255 && PACKAGE.matcher(name).matches();
    }

    // Inverse of the host's escape: \n \r \t and \\ , any other escaped char stands for itself
    public static String unescape(String s) {
        if (s.indexOf('\\') < 0) return s;
        final StringBuilder out = new StringBuilder(s.length());
        for (int i = 0; i < s.length(); i++) {
            final char c = s.charAt(i);
            if (c == '\\' && i + 1 < s.length()) {
                final char n = s.charAt(++i);
                out.append(n == 'n' ? '\n' : n == 'r' ? '\r' : n == 't' ? '\t' : n);
            } else {
                out.append(c);
            }
        }
        return out.toString();
    }

    // Escapes a value for a single protocol line, the inverse of unescape
    public static String escape(String s) {
        final StringBuilder out = new StringBuilder(s.length());
        for (int i = 0; i < s.length(); i++) {
            final char c = s.charAt(i);
            out.append(c == '\n' ? "\\n" : c == '\r' ? "\\r" : c == '\t' ? "\\t" : c == '\\' ? "\\\\" : String.valueOf(c));
        }
        return out.toString();
    }

    // A payload line must never be a lone "." or contain a line break, or it would end the reply early
    public static String payloadLine(String s) {
        final String t = s.replace('\r', ' ').replace('\n', ' ');
        return t.equals(".") ? ".." : t;
    }

    // App labels sit after a TAB separator, so they cannot hold one themselves
    public static String cleanLabel(String s) {
        return s.replace('\r', ' ').replace('\n', ' ').replace('\t', ' ').trim();
    }

    public static String ok(String head, List<String> lines) {
        final StringBuilder sb = new StringBuilder("ok");
        if (head != null && !head.isEmpty()) sb.append(' ').append(payloadLine(head));
        sb.append('\n');
        for (String l : lines) sb.append(payloadLine(l)).append('\n');
        return sb.append(".\n").toString();
    }

    public static String ok(String head) {
        return ok(head, new ArrayList<String>());
    }

    public static String err(String message) {
        return "err " + payloadLine(message == null || message.isEmpty() ? "failed" : message) + "\n.\n";
    }

    // Package and label pairs, sorted by label, one "package TAB label" line each
    public static String appsReply(List<String[]> apps) {
        final List<String[]> sorted = new ArrayList<String[]>(apps);
        java.util.Collections.sort(sorted, new java.util.Comparator<String[]>() {
            @Override
            public int compare(String[] a, String[] b) {
                final int c = a[1].compareToIgnoreCase(b[1]);
                return c != 0 ? c : a[0].compareTo(b[0]);
            }
        });
        final List<String> lines = new ArrayList<String>();
        for (String[] a : sorted) lines.add(a[0] + "\t" + cleanLabel(a[1]));
        return ok(String.valueOf(lines.size()), lines);
    }

    // Splits a byte stream into lines, an overlong line is dropped whole
    public static final class LineSplitter {
        private final ByteArrayOutputStream cur = new ByteArrayOutputStream();
        private boolean overflow;

        public List<String> feed(byte[] buf, int len) {
            final List<String> lines = new ArrayList<String>();
            for (int i = 0; i < len; i++) {
                final byte b = buf[i];
                if (b == '\n') {
                    if (!overflow) lines.add(new String(cur.toByteArray(), java.nio.charset.StandardCharsets.UTF_8));
                    cur.reset();
                    overflow = false;
                } else if (!overflow) {
                    if (cur.size() >= MAX_LINE_BYTES) {
                        overflow = true;
                        cur.reset();
                    } else {
                        cur.write(b);
                    }
                }
            }
            return lines;
        }
    }

    private static float clamp01(float v) {
        return v < 0f ? 0f : (v > 1f ? 1f : v);
    }

    private static Integer intOrNull(String s) {
        try {
            return Integer.valueOf(s);
        } catch (NumberFormatException e) {
            return null;
        }
    }

    private static Float floatOrNull(String s) {
        try {
            final float f = Float.parseFloat(s);
            return Float.isNaN(f) || Float.isInfinite(f) ? null : Float.valueOf(f);
        } catch (NumberFormatException e) {
            return null;
        }
    }
}
