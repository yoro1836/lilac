/* GPU buffers for gralloc.default (gralloc_gpu.c). Return 0 or -errno. */
#pragma once
#include <stddef.h>
#include <stdint.h>

/* struct android_ycbcr */
struct vine_ycbcr {
    void* y;
    void* cb;
    void* cr;
    size_t ystride;
    size_t cstride;
    size_t chroma_step;
    uint32_t reserved[8];
};

int vine_gpu_alloc(int w, int h, int format, int usage, const void** handle, int* stride);
int vine_gpu_free(const void* handle);
int vine_gpu_register(const void* handle);
int vine_gpu_unregister(const void* handle);
int vine_gpu_lock(const void* handle, int usage, void** vaddr);
int vine_gpu_lock_ycbcr(const void* handle, int usage, struct vine_ycbcr* out);
int vine_gpu_unlock(const void* handle);
