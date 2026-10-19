package com.hexadecinull.vineos.agent;

import android.content.ClipData;
import android.content.ClipboardManager;
import android.content.ComponentName;
import android.content.Context;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.content.pm.ResolveInfo;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.drawable.Drawable;
import android.net.LocalServerSocket;
import android.net.LocalSocket;
import android.os.Handler;
import android.os.Looper;
import android.os.SystemClock;
import android.system.Os;
import android.system.OsConstants;
import android.system.UnixSocketAddress;
import android.util.Base64;
import android.util.Log;
import android.view.InputDevice;
import android.view.InputEvent;
import android.view.KeyCharacterMap;
import android.view.KeyEvent;
import android.view.MotionEvent;

import java.io.BufferedReader;
import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileDescriptor;
import java.io.IOException;
import java.io.InputStream;
import java.io.InputStreamReader;
import java.io.OutputStream;
import java.io.RandomAccessFile;
import java.lang.reflect.Method;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.concurrent.Callable;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.FutureTask;
import java.util.concurrent.TimeUnit;

// Guest agent run as root via app_process, serves /vine/agent.sock, unverified on a booted ROM
public final class VineAgent {
    private static final String TAG = "VineAgent";
    private static final String SOCKET = "/vine/agent.sock";
    private static final String FRAMEBUFFER = "/vine/vinefb";
    private static final int ICON_SIZE = 192;

    private final AgentProtocol.PointerTracker tracker = new AgentProtocol.PointerTracker();
    private final ExecutorService workers = Executors.newCachedThreadPool();
    private Handler mainHandler;
    private Context context;
    private Object inputManager;
    private Method injectMethod;
    private long downTime;

    public static void main(String[] args) {
        Looper.prepareMainLooper();
        final VineAgent agent = new VineAgent();
        agent.mainHandler = new Handler(Looper.getMainLooper());
        agent.context = systemContext();
        agent.initInput();
        new Thread(new Runnable() {
            @Override
            public void run() {
                try {
                    agent.serve();
                } catch (Exception e) {
                    Log.e(TAG, "server stopped", e);
                    System.exit(1);
                }
            }
        }, "vine-agent-server").start();
        Looper.loop();
    }

    private static Context systemContext() {
        try {
            final Class<?> at = Class.forName("android.app.ActivityThread");
            final Object thread = at.getMethod("systemMain").invoke(null);
            return (Context) at.getMethod("getSystemContext").invoke(thread);
        } catch (Throwable t) {
            Log.w(TAG, "no system context, falling back to shell tools", t);
            return null;
        }
    }

    private void initInput() {
        for (String name : new String[] {"android.hardware.input.InputManagerGlobal", "android.hardware.input.InputManager"}) {
            try {
                final Class<?> c = Class.forName(name);
                final Method getInstance = c.getDeclaredMethod("getInstance");
                getInstance.setAccessible(true);
                final Object manager = getInstance.invoke(null);
                final Method inject = c.getMethod("injectInputEvent", InputEvent.class, int.class);
                inject.setAccessible(true);
                inputManager = manager;
                injectMethod = inject;
                return;
            } catch (Throwable t) {
                Log.w(TAG, name + " unavailable: " + t);
            }
        }
        Log.e(TAG, "no way to inject input, touch and keys will be ignored");
    }

    private void serve() throws Exception {
        new File(SOCKET).delete();
        final FileDescriptor fd = Os.socket(OsConstants.AF_UNIX, OsConstants.SOCK_STREAM, 0);
        Os.bind(fd, UnixSocketAddress.createFileSystem(SOCKET));
        Os.chmod(SOCKET, 0666);
        final LocalServerSocket server = new LocalServerSocket(fd);
        Log.i(TAG, "listening on " + SOCKET);
        for (;;) {
            final LocalSocket client = server.accept();
            new Thread(new Runnable() {
                @Override
                public void run() {
                    session(client);
                }
            }, "vine-agent-session").start();
        }
    }

    private void session(LocalSocket client) {
        final AgentProtocol.LineSplitter splitter = new AgentProtocol.LineSplitter();
        final Object writeLock = new Object();
        try {
            final InputStream in = client.getInputStream();
            final OutputStream out = client.getOutputStream();
            final byte[] buf = new byte[8192];
            int n;
            while ((n = in.read(buf)) > 0) {
                for (String line : splitter.feed(buf, n)) handle(line, out, writeLock);
            }
        } catch (IOException e) {
            Log.i(TAG, "session ended: " + e.getMessage());
        } finally {
            tracker.clear();
            try {
                client.close();
            } catch (IOException ignored) {
                // nothing left to close
            }
        }
    }

    private void handle(String line, final OutputStream out, final Object writeLock) {
        final AgentProtocol.Command cmd = AgentProtocol.parse(line);
        if (cmd == null) return;
        switch (cmd.name) {
            case "touch":
                final AgentProtocol.Touch t = AgentProtocol.parseTouch(cmd.rest);
                if (t != null) touch(t);
                return;
            case "key":
                final String[] w = cmd.words();
                if (w.length == 2 && AgentProtocol.validKeycode(parseInt(w[0]))) key(parseInt(w[0]), "1".equals(w[1]));
                return;
            case "text":
                typeText(AgentProtocol.unescape(cmd.rest));
                return;
            default:
                workers.execute(new Runnable() {
                    @Override
                    public void run() {
                        reply(out, writeLock, answer(cmd));
                    }
                });
        }
    }

    private void reply(OutputStream out, Object lock, String text) {
        try {
            synchronized (lock) {
                out.write(text.getBytes(StandardCharsets.UTF_8));
                out.flush();
            }
        } catch (IOException e) {
            Log.w(TAG, "reply failed: " + e.getMessage());
        }
    }

    private String answer(AgentProtocol.Command cmd) {
        try {
            switch (cmd.name) {
                case "ping":
                    return AgentProtocol.ok("pong");
                case "apps":
                    return AgentProtocol.appsReply(listApps());
                case "launch":
                    return launch(cmd.rest.trim());
                case "icon":
                    return icon(cmd.rest.trim());
                case "clip":
                    return clip(cmd.rest);
                default:
                    return AgentProtocol.err("unknown command " + cmd.name);
            }
        } catch (Throwable t) {
            Log.w(TAG, cmd.name + " failed", t);
            return AgentProtocol.err(cmd.name + " failed: " + t);
        }
    }

    // Input

    private void touch(AgentProtocol.Touch t) {
        if (injectMethod == null) return;
        final boolean pointerEvent = t.action == MotionEvent.ACTION_POINTER_DOWN || t.action == MotionEvent.ACTION_POINTER_UP;
        if (t.action == MotionEvent.ACTION_DOWN) tracker.clear();
        final int[] slots = tracker.map(t.ids);
        final int[] size = displaySize();
        final int n = t.ids.length;
        final MotionEvent.PointerProperties[] props = new MotionEvent.PointerProperties[n];
        final MotionEvent.PointerCoords[] coords = new MotionEvent.PointerCoords[n];
        for (int i = 0; i < n; i++) {
            props[i] = new MotionEvent.PointerProperties();
            props[i].id = slots[i];
            props[i].toolType = MotionEvent.TOOL_TYPE_FINGER;
            coords[i] = new MotionEvent.PointerCoords();
            coords[i].x = t.x[i] * (size[0] - 1);
            coords[i].y = t.y[i] * (size[1] - 1);
            coords[i].pressure = 1f;
            coords[i].size = 1f;
        }
        final long now = SystemClock.uptimeMillis();
        if (t.action == MotionEvent.ACTION_DOWN) downTime = now;
        final int action = pointerEvent ? t.action | (t.actionIndex << MotionEvent.ACTION_POINTER_INDEX_SHIFT) : t.action;
        final MotionEvent event = MotionEvent.obtain(downTime, now, action, n, props, coords, 0, 0, 1f, 1f, 0, 0, InputDevice.SOURCE_TOUCHSCREEN, 0);
        inject(event);
        event.recycle();
        if (t.action == MotionEvent.ACTION_UP || t.action == MotionEvent.ACTION_CANCEL) tracker.clear();
        else if (t.action == MotionEvent.ACTION_POINTER_UP) tracker.release(t.ids[t.actionIndex]);
    }

    // The framebuffer header holds the guest screen size, so touch needs no extra configuration
    private int[] displaySize() {
        try (RandomAccessFile f = new RandomAccessFile(FRAMEBUFFER, "r")) {
            final byte[] head = new byte[16];
            f.readFully(head);
            final ByteBuffer b = ByteBuffer.wrap(head).order(ByteOrder.LITTLE_ENDIAN);
            final int w = b.getInt(8);
            final int h = b.getInt(12);
            if (w > 0 && h > 0) return new int[] {w, h};
        } catch (IOException ignored) {
            // fall through to the default
        }
        return new int[] {1080, 1920};
    }

    private void key(int code, boolean down) {
        final long now = SystemClock.uptimeMillis();
        inject(new KeyEvent(now, now, down ? KeyEvent.ACTION_DOWN : KeyEvent.ACTION_UP, code, 0, 0, KeyCharacterMap.VIRTUAL_KEYBOARD, 0, 0, InputDevice.SOURCE_KEYBOARD));
    }

    private void typeText(String text) {
        final KeyCharacterMap map = KeyCharacterMap.load(KeyCharacterMap.VIRTUAL_KEYBOARD);
        for (int i = 0; i < text.length(); i++) {
            final KeyEvent[] events = map.getEvents(new char[] {text.charAt(i)});
            if (events == null) continue;
            for (KeyEvent e : events) {
                setSource(e, InputDevice.SOURCE_KEYBOARD);
                inject(e);
            }
        }
    }

    private void inject(InputEvent event) {
        if (injectMethod == null) return;
        try {
            injectMethod.invoke(inputManager, event, 0);
        } catch (Throwable t) {
            Log.w(TAG, "inject failed: " + t);
        }
    }

    private static void setSource(InputEvent event, int source) {
        try {
            final Method m = InputEvent.class.getMethod("setSource", int.class);
            m.invoke(event, source);
        } catch (Throwable ignored) {
            // the default source still works on most builds
        }
    }

    // Apps, launch, icons, clipboard

    private List<String[]> listApps() throws IOException {
        final Map<String, String[]> found = new LinkedHashMap<String, String[]>();
        if (context != null) {
            final PackageManager pm = context.getPackageManager();
            final Intent probe = new Intent(Intent.ACTION_MAIN).addCategory(Intent.CATEGORY_LAUNCHER);
            for (ResolveInfo ri : pm.queryIntentActivities(probe, 0)) {
                final String pkg = ri.activityInfo.packageName;
                if (!found.containsKey(pkg)) found.put(pkg, new String[] {pkg, String.valueOf(ri.loadLabel(pm))});
            }
        } else {
            for (String line : run("pm", "list", "packages", "-3").split("\n")) {
                final String pkg = line.startsWith("package:") ? line.substring(8).trim() : "";
                if (AgentProtocol.validPackage(pkg)) found.put(pkg, new String[] {pkg, pkg});
            }
        }
        return new ArrayList<String[]>(found.values());
    }

    private String launch(String pkg) throws IOException {
        if (!AgentProtocol.validPackage(pkg)) return AgentProtocol.err("bad package name");
        ComponentName component = null;
        if (context != null) {
            final Intent li = context.getPackageManager().getLaunchIntentForPackage(pkg);
            if (li == null) return AgentProtocol.err("no launcher activity for " + pkg);
            component = li.getComponent();
        }
        final String out = component != null
                ? run("am", "start", "-a", Intent.ACTION_MAIN, "-c", Intent.CATEGORY_LAUNCHER, "-n", component.flattenToShortString())
                : run("monkey", "-p", pkg, "-c", Intent.CATEGORY_LAUNCHER, "1");
        return out.contains("Error") ? AgentProtocol.err(firstLine(out)) : AgentProtocol.ok("");
    }

    private String icon(String pkg) throws Exception {
        if (!AgentProtocol.validPackage(pkg)) return AgentProtocol.err("bad package name");
        if (context == null) return AgentProtocol.err("icons need the system context");
        final Drawable d = context.getPackageManager().getApplicationIcon(pkg);
        final Bitmap bmp = Bitmap.createBitmap(ICON_SIZE, ICON_SIZE, Bitmap.Config.ARGB_8888);
        d.setBounds(0, 0, ICON_SIZE, ICON_SIZE);
        d.draw(new Canvas(bmp));
        final ByteArrayOutputStream png = new ByteArrayOutputStream();
        bmp.compress(Bitmap.CompressFormat.PNG, 100, png);
        bmp.recycle();
        final List<String> lines = new ArrayList<String>();
        lines.add(Base64.encodeToString(png.toByteArray(), Base64.NO_WRAP));
        return AgentProtocol.ok("", lines);
    }

    private String clip(String rest) throws Exception {
        if (context == null) return AgentProtocol.err("clipboard needs the system context");
        final boolean set = rest.startsWith("set");
        final String value = set ? AgentProtocol.unescape(rest.length() > 4 ? rest.substring(4) : "") : "";
        final String result = onMain(new Callable<String>() {
            @Override
            public String call() {
                final ClipboardManager cm = (ClipboardManager) context.getSystemService(Context.CLIPBOARD_SERVICE);
                if (set) {
                    cm.setPrimaryClip(ClipData.newPlainText("vine", value));
                    return "";
                }
                final ClipData clip = cm.getPrimaryClip();
                return clip != null && clip.getItemCount() > 0 ? String.valueOf(clip.getItemAt(0).coerceToText(context)) : "";
            }
        });
        final List<String> lines = new ArrayList<String>();
        if (!set) lines.add(AgentProtocol.escape(result));
        return AgentProtocol.ok("", lines);
    }

    private <T> T onMain(Callable<T> job) throws Exception {
        final FutureTask<T> task = new FutureTask<T>(job);
        mainHandler.post(task);
        return task.get(5, TimeUnit.SECONDS);
    }

    private static String run(String... command) throws IOException {
        final Process p = new ProcessBuilder(command).redirectErrorStream(true).start();
        final StringBuilder sb = new StringBuilder();
        try (BufferedReader r = new BufferedReader(new InputStreamReader(p.getInputStream(), StandardCharsets.UTF_8))) {
            String l;
            while ((l = r.readLine()) != null) sb.append(l).append('\n');
        }
        try {
            p.waitFor();
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
        }
        return sb.toString();
    }

    private static String firstLine(String s) {
        final int nl = s.indexOf('\n');
        return nl < 0 ? s : s.substring(0, nl);
    }

    private static int parseInt(String s) {
        try {
            return Integer.parseInt(s);
        } catch (NumberFormatException e) {
            return -1;
        }
    }
}
