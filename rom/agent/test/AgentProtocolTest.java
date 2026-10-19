import com.hexadecinull.vineos.agent.AgentProtocol;
import com.hexadecinull.vineos.agent.AgentProtocol.Command;
import com.hexadecinull.vineos.agent.AgentProtocol.Touch;

import java.nio.charset.StandardCharsets;
import java.util.Arrays;
import java.util.List;

// Plain main() runner so the check needs nothing but a JDK
public class AgentProtocolTest {
    static int checks, failed;

    static void check(boolean ok, String what) {
        checks++;
        if (!ok) {
            failed++;
            System.out.println("  FAIL " + what);
        }
    }

    public static void main(String[] args) {
        Command c = AgentProtocol.parse("launch com.example.app");
        check(c != null && c.name.equals("launch") && c.rest.equals("com.example.app"), "parse with argument");
        check(AgentProtocol.parse("ping").rest.isEmpty(), "parse without argument");
        check(AgentProtocol.parse("   ") == null && AgentProtocol.parse(null) == null, "blank lines");
        check(AgentProtocol.parse("text a b  c").rest.equals("a b  c"), "rest keeps inner spaces");

        Touch t = AgentProtocol.parseTouch("0 0 1 17 0.5000 0.2500");
        check(t != null && t.action == 0 && t.ids[0] == 17 && t.x[0] == 0.5f && t.y[0] == 0.25f, "single touch");
        t = AgentProtocol.parseTouch("5 1 2 3 0.1 0.2 9 1.5 -0.5");
        check(t != null && t.actionIndex == 1 && t.ids.length == 2 && t.x[1] == 1f && t.y[1] == 0f, "two pointers, clamped");
        check(AgentProtocol.parseTouch("0 0 1 1 0.5") == null, "short touch");
        check(AgentProtocol.parseTouch("0 0 2 1 0.5 0.5") == null, "count mismatch");
        check(AgentProtocol.parseTouch("0 2 2 1 0.5 0.5 2 0.5 0.5") == null, "action index out of range");
        check(AgentProtocol.parseTouch("0 0 0") == null, "zero pointers");
        check(AgentProtocol.parseTouch("0 0 1 1 NaN 0.5") == null, "NaN");
        check(AgentProtocol.parseTouch("0 0 1 1 abc 0.5") == null, "not a number");
        StringBuilder many = new StringBuilder("0 0 11");
        for (int i = 0; i < 11; i++) many.append(" ").append(i).append(" 0.5 0.5");
        check(AgentProtocol.parseTouch(many.toString()) == null, "too many pointers");
        check(AgentProtocol.parseTouch("") == null, "empty touch");

        AgentProtocol.PointerTracker p = new AgentProtocol.PointerTracker();
        int[] a = p.map(new int[] {9001});
        check(a[0] == 0, "first pointer takes slot 0");
        int[] b = p.map(new int[] {9001, 42});
        check(b[0] == 0 && b[1] == 1, "ids are stable and the second pointer takes slot 1");
        p.release(9001);
        int[] d = p.map(new int[] {42, 7});
        check(d[0] == 1 && d[1] == 0, "a freed slot is reused");
        p.clear();
        check(p.map(new int[] {5})[0] == 0, "clear frees everything");
        int[] ids = new int[AgentProtocol.MAX_POINTERS + 2];
        for (int i = 0; i < ids.length; i++) ids[i] = 100 + i;
        int[] m = new AgentProtocol.PointerTracker().map(ids);
        boolean inRange = true;
        for (int v : m) inRange &= v >= 0 && v < AgentProtocol.MAX_POINTERS;
        check(inRange, "slots stay in range when the host sends too many pointers");

        check(AgentProtocol.validPackage("com.example.app_2") && AgentProtocol.validPackage("a.b"), "valid packages");
        check(!AgentProtocol.validPackage("com") && !AgentProtocol.validPackage("com.example; rm -rf /") && !AgentProtocol.validPackage("../x.y")
                && !AgentProtocol.validPackage("") && !AgentProtocol.validPackage(null) && !AgentProtocol.validPackage("1a.b"), "invalid packages");
        check(AgentProtocol.validKeycode(4) && AgentProtocol.validKeycode(66) && !AgentProtocol.validKeycode(0) && !AgentProtocol.validKeycode(-1)
                && !AgentProtocol.validKeycode(5000), "keycode range");

        check(AgentProtocol.unescape("a\\tb\\nc\\\\d\\x").equals("a\tb\nc\\dx"), "unescape");
        check(AgentProtocol.unescape(AgentProtocol.escape("a\tb\r\nc\\d")).equals("a\tb\r\nc\\d"), "escape round trip");
        check(AgentProtocol.escape("x\ny").equals("x\\ny") && AgentProtocol.escape("p\\q").equals("p\\\\q"), "escape output");
        check(AgentProtocol.unescape("plain").equals("plain") && AgentProtocol.unescape("end\\").equals("end\\"), "unescape edge cases");

        check(AgentProtocol.ok("pong").equals("ok pong\n.\n"), "ok with head");
        check(AgentProtocol.ok("").equals("ok\n.\n"), "ok without head");
        check(AgentProtocol.err("no such package").equals("err no such package\n.\n"), "err");
        check(AgentProtocol.err(null).equals("err failed\n.\n"), "err without message");
        check(AgentProtocol.err("two\nlines").equals("err two lines\n.\n"), "err cannot break the frame");
        check(AgentProtocol.ok("x", Arrays.asList(".", "a\nb")).equals("ok x\n..\na b\n.\n"), "payload lines cannot end the reply");

        List<String[]> apps = Arrays.asList(new String[] {"com.z", "zeta"}, new String[] {"com.b", "Beta"}, new String[] {"com.a", "alpha\t."});
        String r = AgentProtocol.appsReply(apps);
        check(r.equals("ok 3\ncom.a\talpha .\ncom.b\tBeta\ncom.z\tzeta\n.\n"), "apps reply sorted: " + r.replace("\n", "|"));
        check(AgentProtocol.appsReply(Arrays.<String[]>asList()).equals("ok 0\n.\n"), "empty apps reply");
        check(AgentProtocol.appsReply(Arrays.<String[]>asList(new String[] {"com.a", "."})).equals("ok 1\ncom.a\t.\n.\n"), "a label of one dot is safe behind the package");
        check(AgentProtocol.appsReply(Arrays.<String[]>asList(new String[] {"com.a", ""})).equals("ok 1\ncom.a\t\n.\n"), "an empty label stays a valid line");
        check(AgentProtocol.ok("t", Arrays.asList("keep\ttab")).equals("ok t\nkeep\ttab\n.\n"), "payload lines keep tabs");

        AgentProtocol.LineSplitter s = new AgentProtocol.LineSplitter();
        byte[] part1 = "ping\nkey 4 1\nte".getBytes(StandardCharsets.UTF_8);
        List<String> got = s.feed(part1, part1.length);
        check(got.size() == 2 && got.get(0).equals("ping") && got.get(1).equals("key 4 1"), "lines from the first chunk");
        byte[] part2 = "xt héllo\n".getBytes(StandardCharsets.UTF_8);
        got = s.feed(part2, part2.length);
        check(got.size() == 1 && got.get(0).equals("text héllo"), "a line split across reads, utf-8 intact");
        byte[] big = new byte[AgentProtocol.MAX_LINE_BYTES + 10];
        Arrays.fill(big, (byte) 'a');
        got = s.feed(big, big.length);
        check(got.isEmpty(), "an overlong line is not returned");
        byte[] tail = "\nping\n".getBytes(StandardCharsets.UTF_8);
        got = s.feed(tail, tail.length);
        check(got.size() == 1 && got.get(0).equals("ping"), "the splitter recovers after an overlong line");

        System.out.println(checks + " checks, " + failed + " failed");
        System.exit(failed == 0 ? 0 : 1);
    }
}
