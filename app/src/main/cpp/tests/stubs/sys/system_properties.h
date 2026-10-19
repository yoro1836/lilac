#pragma once
// Host-build stub of <sys/system_properties.h>: every property reads as empty.
#include <cstring>
#define PROP_VALUE_MAX 92
inline int __system_property_get(const char*, char* value) {
    value[0] = '\0';
    return 0;
}
