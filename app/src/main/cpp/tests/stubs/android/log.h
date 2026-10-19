#pragma once
// Host-build stub of <android/log.h>, only used by the Linux native test build.
#include <cstdarg>
#include <cstdio>

enum { ANDROID_LOG_UNKNOWN = 0, ANDROID_LOG_DEFAULT, ANDROID_LOG_VERBOSE, ANDROID_LOG_DEBUG,
       ANDROID_LOG_INFO, ANDROID_LOG_WARN, ANDROID_LOG_ERROR, ANDROID_LOG_FATAL };

inline int __android_log_print(int prio, const char* tag, const char* fmt, ...) {
    static const char kLevels[] = "?-VDIWEF";
    if (!getenv("VINE_TEST_QUIET")) {
        std::fprintf(stderr, "%c/%s: ", kLevels[prio < 0 || prio > 7 ? 0 : prio], tag);
        va_list ap;
        va_start(ap, fmt);
        std::vfprintf(stderr, fmt, ap);
        va_end(ap);
        std::fputc('\n', stderr);
    }
    return 0;
}
