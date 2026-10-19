#pragma once
struct AChoreographer;
typedef void (*AChoreographer_frameCallback)(long frameTimeNanos, void* data);
inline AChoreographer* AChoreographer_getInstance() { return nullptr; }
inline void AChoreographer_postFrameCallback(AChoreographer*, AChoreographer_frameCallback, void*) {}
