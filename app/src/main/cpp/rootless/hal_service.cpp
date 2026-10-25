// vine_hal_service IFACE...: serves passthrough HIDL implementations
// as their own (vendor) process, e.g.
//   vine_hal_service android.hardware.audio@4.0::IDevicesFactory \
//                    android.hardware.audio.effect@4.0::IEffectsFactory
// Stands in for a ROM's 32-bit-only HAL services on hosts without
// AArch32: the 64-bit implementations are in the image, only their
// service executables are not. Runs inside the guest and dlopen()s the
// guest's libhidlbase, whose libc++ namespace is std::__1 (the NDK's is
// std::__ndk1). The string layout is the same in both, so the NDK's
// strings are passed straight to the platform's functions, which only
// read them.
#include <cstddef>
#include <cstdio>
#include <dlfcn.h>
#include <signal.h>
#include <string>

namespace {

using RegisterFn = int (*)(const std::string&, const std::string&, const std::string&);
using ConfigureFn = void (*)(size_t, bool);
using JoinFn = void (*)();

// android::hardware::registerPassthroughServiceImplementation(const std::string&, const std::string&, const std::string&)
const char* const kRegister =
    "_ZN7android8hardware40registerPassthroughServiceImplementationERKNSt3__112basic_stringIcNS1_11char_traitsIcEENS1_"
    "9allocatorIcEEEES9_S9_";
// android::hardware::configureRpcThreadpool(size_t, bool)
const char* const kConfigure = "_ZN7android8hardware22configureRpcThreadpoolEmb";
// android::hardware::joinRpcThreadpool()
const char* const kJoin = "_ZN7android8hardware17joinRpcThreadpoolEv";

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: vine_hal_service IFACE...\n");
        return 2;
    }
    signal(SIGPIPE, SIG_IGN);
    void* hidl = dlopen("libhidlbase.so", RTLD_NOW);
    auto reg = hidl ? reinterpret_cast<RegisterFn>(dlsym(hidl, kRegister)) : nullptr;
    auto configure = hidl ? reinterpret_cast<ConfigureFn>(dlsym(hidl, kConfigure)) : nullptr;
    auto join = hidl ? reinterpret_cast<JoinFn>(dlsym(hidl, kJoin)) : nullptr;
    if (!reg || !configure || !join) {
        fprintf(stderr, "vine_hal_service: libhidlbase: %s\n", dlerror());
        return 1;
    }
    configure(16, true);
    int registered = 0;
    for (int i = 1; i < argc; ++i) {
        const std::string iface = argv[i];
        if (reg(iface, iface, "default") == 0) {
            ++registered;
        } else {
            fprintf(stderr, "vine_hal_service: cannot register %s\n", argv[i]);
        }
    }
    if (registered == 0) return 1;
    join();
    return 1;
}
