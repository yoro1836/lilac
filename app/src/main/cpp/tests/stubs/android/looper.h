#pragma once
struct ALooper;
inline ALooper* ALooper_prepare(int) { return nullptr; }
inline int ALooper_pollOnce(int, int*, int*, void**) { return 0; }
