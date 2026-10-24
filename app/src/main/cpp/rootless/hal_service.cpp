// vine_hal_service IFACE...: serves passthrough HIDL implementations
// as their own (vendor) process, e.g.
//   vine_hal_service android.hardware.audio@4.0::IDevicesFactory \
//                    android.hardware.audio.effect@4.0::IEffectsFactory
// Stands in for a ROM's 32-bit-only HAL services on hosts without
// AArch32: the 64-bit implementations are in the image, only their
// service executables are not. Runs inside the guest and links against
// the guest's libhidlbase, so it is built for the platform's libc++ ABI
// (std::__1), see scripts/rootless/README.md.
#include <cstddef>
#include <cstdio>
#include <signal.h>
#include <string>

namespace android::hardware {
int registerPassthroughServiceImplementation(const std::string& interfaceName,
                                             const std::string& expectInterfaceName,
                                             const std::string& serviceName);
void configureRpcThreadpool(size_t maxThreads, bool callerWillJoin);
void joinRpcThreadpool();
} // namespace android::hardware

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: vine_hal_service IFACE...\n");
        return 2;
    }
    signal(SIGPIPE, SIG_IGN);
    android::hardware::configureRpcThreadpool(16, true);
    int registered = 0;
    for (int i = 1; i < argc; ++i) {
        const std::string iface = argv[i];
        if (android::hardware::registerPassthroughServiceImplementation(iface, iface, "default") == 0) {
            ++registered;
        } else {
            fprintf(stderr, "vine_hal_service: cannot register %s\n", argv[i]);
        }
    }
    if (registered == 0) return 1;
    android::hardware::joinRpcThreadpool();
    return 1;
}
