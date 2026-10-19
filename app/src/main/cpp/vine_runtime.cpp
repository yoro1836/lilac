#include <jni.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>

#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

#include "diagnostics/host_probe.h"
#include "diagnostics/namespace_probe.h"
#include "display/display_bridge.h"
#include "qemu_bridge/qemu_launcher.h"
#include "utils/vine_log.h"
#include "utils/vine_utils.h"

// JNI surface for what needs no root: device probes and frames to a Surface, guest control is in helper/

namespace {

std::mutex g_mutex;
std::unordered_map<jlong, std::unique_ptr<vine::display::DisplayBridge>> g_displays;
jlong g_next_display = 1;

std::string j2s(JNIEnv* env, jstring js) {
    if (!js) return {};
    const char* c = env->GetStringUTFChars(js, nullptr);
    std::string s(c ? c : "");
    if (c) env->ReleaseStringUTFChars(js, c);
    return s;
}

jstring s2j(JNIEnv* env, const std::string& s) { return env->NewStringUTF(s.c_str()); }

// Looks up a display and runs fn on it while the registry lock is held, so closeDisplay can't free it mid-call
template <typename Fn>
auto with_display(jlong handle, Fn fn, decltype(fn(std::declval<vine::display::DisplayBridge&>())) fallback) {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_displays.find(handle);
    return it == g_displays.end() ? fallback : fn(*it->second);
}

} // namespace

extern "C" {

JNIEXPORT jstring JNICALL
Java_com_hexadecinull_vineos_native_VineRuntime_getRuntimeVersion(JNIEnv* env, jobject) {
    return s2j(env, "VineRuntime/0.2.0 (C++17)");
}

JNIEXPORT jboolean JNICALL
Java_com_hexadecinull_vineos_native_VineRuntime_hostSupportsAArch32(JNIEnv*, jobject) {
    return vine::host_supports_aarch32() ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jstring JNICALL
Java_com_hexadecinull_vineos_native_VineRuntime_probeHost(JNIEnv* env, jobject) {
    return s2j(env, vine::diagnostics::capabilities_to_text(vine::diagnostics::probe_host()));
}

JNIEXPORT jboolean JNICALL
Java_com_hexadecinull_vineos_native_VineRuntime_verifyQemu(JNIEnv* env, jobject, jstring j_path) {
    return vine::qemu::verify_qemu_binary(j2s(env, j_path)) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jlong JNICALL
Java_com_hexadecinull_vineos_native_VineRuntime_openDisplay(JNIEnv* env, jobject, jstring j_path) {
    auto display = std::make_unique<vine::display::DisplayBridge>();
    std::string error;
    if (!display->open(j2s(env, j_path), &error)) {
        VINE_LOGW("openDisplay: %s", error.c_str());
        return 0;
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    const jlong handle = g_next_display++;
    g_displays[handle] = std::move(display);
    return handle;
}

JNIEXPORT void JNICALL
Java_com_hexadecinull_vineos_native_VineRuntime_closeDisplay(JNIEnv*, jobject, jlong handle) {
    std::unique_ptr<vine::display::DisplayBridge> doomed;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto it = g_displays.find(handle);
        if (it == g_displays.end()) return;
        doomed = std::move(it->second);
        g_displays.erase(it);
    }
    doomed->close(); // joins the render thread outside the lock
}

JNIEXPORT void JNICALL
Java_com_hexadecinull_vineos_native_VineRuntime_attachSurface(JNIEnv* env, jobject, jlong handle, jobject surface) {
    ANativeWindow* window = surface ? ANativeWindow_fromSurface(env, surface) : nullptr;
    with_display(handle, [&](vine::display::DisplayBridge& d) { d.set_surface(window); return 0; }, 0);
    if (window) ANativeWindow_release(window); // the bridge holds its own reference
}

JNIEXPORT void JNICALL
Java_com_hexadecinull_vineos_native_VineRuntime_detachSurface(JNIEnv*, jobject, jlong handle) {
    with_display(handle, [](vine::display::DisplayBridge& d) { d.set_surface(nullptr); return 0; }, 0);
}

JNIEXPORT jboolean JNICALL
Java_com_hexadecinull_vineos_native_VineRuntime_startRendering(JNIEnv*, jobject, jlong handle) {
    return with_display(handle, [](vine::display::DisplayBridge& d) { return d.start(); }, false) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL
Java_com_hexadecinull_vineos_native_VineRuntime_stopRendering(JNIEnv*, jobject, jlong handle) {
    with_display(handle, [](vine::display::DisplayBridge& d) { d.stop(); return 0; }, 0);
}

// {open, rendering, width, height, guest frames, frames drawn}
JNIEXPORT jlongArray JNICALL
Java_com_hexadecinull_vineos_native_VineRuntime_displayInfo(JNIEnv* env, jobject, jlong handle) {
    jlong out[6] = {0, 0, 0, 0, 0, 0};
    with_display(handle, [&](vine::display::DisplayBridge& d) {
        const auto i = d.info();
        out[0] = i.open ? 1 : 0;
        out[1] = i.rendering ? 1 : 0;
        out[2] = i.width;
        out[3] = i.height;
        out[4] = i.guest_frames;
        out[5] = (jlong)i.frames_drawn;
        return 0;
    }, 0);
    jlongArray arr = env->NewLongArray(6);
    env->SetLongArrayRegion(arr, 0, 6, out);
    return arr;
}

JNIEXPORT jstring JNICALL
Java_com_hexadecinull_vineos_shizuku_VineShizukuService_nativeProbeNamespaces(JNIEnv* env, jobject) {
    const auto r = vine::diagnostics::probe_namespace_support();
    char buf[256];
    snprintf(buf, sizeof(buf), "unshare_ok=%d;unshare_errno=%d;mount_ok=%d;mount_errno=%d;uid=%d", r.unshare_ok ? 1 : 0,
             r.unshare_errno, r.mount_ok ? 1 : 0, r.mount_errno, r.caller_uid);
    return env->NewStringUTF(buf);
}

} // extern "C"
