/* gralloc.default, replaced: the image's gralloc.default with buffer heights padded.
 *
 * gralloc.default sizes a buffer as stride * height, page rounded. The
 * guest renders with lavapipe (ANGLE over Vulkan), which writes whole
 * 4x4 pixel blocks: on a surface whose height is not a multiple of 4
 * (the 42-row status bar) the last block row lands past the buffer, in
 * the next mapping, and the app dies with SEGV_ACCERR. Allocating a few
 * more rows gives those writes somewhere to go; consumers still use the
 * buffer's own width and height.
 *
 * Everything else forwards to the original, moved to
 * gralloc.default_real.so, with its own module; this one carries its
 * name and version, which the hwcomposer and Mesa look at. */
#include <dlfcn.h>
#include <stdarg.h>
#include <stdint.h>
#include <string.h>

#define PAD_ROWS 16

/* hardware/hardware.h and hardware/gralloc.h, which the NDK lacks */
struct hw_module_t;
struct hw_device_t;
typedef struct hw_module_methods_t {
    int (*open)(const struct hw_module_t*, const char* id, struct hw_device_t** device);
} hw_module_methods_t;
typedef struct hw_module_t {
    uint32_t tag;
    uint16_t module_api_version;
    uint16_t hal_api_version;
    const char* id;
    const char* name;
    const char* author;
    hw_module_methods_t* methods;
    void* dso;
    uint64_t reserved[32 - 7];
} hw_module_t;
typedef struct hw_device_t {
    uint32_t tag;
    uint32_t version;
    struct hw_module_t* module;
    uint64_t reserved[12];
    int (*close)(struct hw_device_t* device);
} hw_device_t;
typedef const void* buffer_handle_t;
typedef struct gralloc_module_t {
    hw_module_t common;
    int (*registerBuffer)(const struct gralloc_module_t*, buffer_handle_t);
    int (*unregisterBuffer)(const struct gralloc_module_t*, buffer_handle_t);
    int (*lock)(const struct gralloc_module_t*, buffer_handle_t, int, int, int, int, int, void**);
    int (*unlock)(const struct gralloc_module_t*, buffer_handle_t);
    int (*perform)(const struct gralloc_module_t*, int, ...);
    int (*lock_ycbcr)(const struct gralloc_module_t*, buffer_handle_t, int, int, int, int, int, void*);
    int (*lockAsync)(const struct gralloc_module_t*, buffer_handle_t, int, int, int, int, int, void**, int);
    int (*unlockAsync)(const struct gralloc_module_t*, buffer_handle_t, int*);
    int (*lockAsync_ycbcr)(const struct gralloc_module_t*, buffer_handle_t, int, int, int, int, int, void*, int);
    int32_t (*getTransportSize)(const struct gralloc_module_t*, buffer_handle_t, uint32_t*, uint32_t*);
    int32_t (*validateBufferSize)(const struct gralloc_module_t*, buffer_handle_t, uint32_t, uint32_t, int32_t,
                                  int, uint32_t, uint32_t);
    void* reserved_proc[1];
} gralloc_module_t;
typedef struct alloc_device_t {
    hw_device_t common;
    int (*alloc)(struct alloc_device_t*, int w, int h, int format, int usage, buffer_handle_t* handle, int* stride);
    int (*free)(struct alloc_device_t*, buffer_handle_t);
    void (*dump)(struct alloc_device_t*, char*, int);
    void* reserved_proc[7];
} alloc_device_t;

#define GRALLOC_HARDWARE_GPU0 "gpu0"

static const gralloc_module_t* real;
static int (*real_alloc)(alloc_device_t*, int, int, int, int, buffer_handle_t*, int*);

static const gralloc_module_t* load(void) {
    if (real) return real;
    void* dso = dlopen("/vendor/lib64/hw/gralloc.default_real.so", RTLD_NOW);
    if (!dso) return 0;
    real = (const gralloc_module_t*)dlsym(dso, "HMI");
    return real;
}

static int padded_alloc(alloc_device_t* dev, int w, int h, int format, int usage, buffer_handle_t* handle,
                        int* stride) {
    return real_alloc(dev, w, (h + PAD_ROWS - 1) / PAD_ROWS * PAD_ROWS, format, usage, handle, stride);
}

static int shim_open(const hw_module_t* module, const char* id, hw_device_t** device) {
    (void)module;
    if (!load()) return -19;
    int r = real->common.methods->open(&real->common, id, device);
    if (r == 0 && !strcmp(id, GRALLOC_HARDWARE_GPU0)) {
        alloc_device_t* d = (alloc_device_t*)*device;
        if (d->alloc != padded_alloc) {
            real_alloc = d->alloc;
            d->alloc = padded_alloc;
        }
    }
    return r;
}

static int shim_register(const gralloc_module_t* m, buffer_handle_t h) {
    (void)m;
    return load() ? real->registerBuffer(real, h) : -19;
}

static int shim_unregister(const gralloc_module_t* m, buffer_handle_t h) {
    (void)m;
    return load() ? real->unregisterBuffer(real, h) : -19;
}

static int shim_lock(const gralloc_module_t* m, buffer_handle_t h, int usage, int l, int t, int w, int hh,
                     void** vaddr) {
    (void)m;
    return load() ? real->lock(real, h, usage, l, t, w, hh, vaddr) : -19;
}

static int shim_unlock(const gralloc_module_t* m, buffer_handle_t h) {
    (void)m;
    return load() ? real->unlock(real, h) : -19;
}

static int shim_perform(const gralloc_module_t* m, int op, ...) {
    (void)m;
    if (!load() || !real->perform) return -22;
    va_list ap;
    va_start(ap, op);
    intptr_t a[6];
    for (int i = 0; i < 6; ++i) a[i] = va_arg(ap, intptr_t);
    va_end(ap);
    return real->perform(real, op, a[0], a[1], a[2], a[3], a[4], a[5]);
}

static hw_module_methods_t shim_methods = { shim_open };

__attribute__((visibility("default"))) gralloc_module_t HMI = {
    .common = {
        .tag = 0x48574d54, /* HARDWARE_MODULE_TAG */
        .module_api_version = 1,
        .hal_api_version = 0,
        .id = "gralloc",
        .name = "Wayondroid padded gralloc",
        .author = "Wayondroid",
        .methods = &shim_methods,
    },
    .registerBuffer = shim_register,
    .unregisterBuffer = shim_unregister,
    .lock = shim_lock,
    .unlock = shim_unlock,
};

/* Offer only what gralloc.default offers: Mesa reads buffer details
 * through perform() when a module has one. */
__attribute__((constructor)) static void init(void) {
    if (!load()) return;
    HMI.common.module_api_version = real->common.module_api_version;
    HMI.common.hal_api_version = real->common.hal_api_version;
    HMI.common.name = real->common.name;
    HMI.common.author = real->common.author;
    HMI.perform = real->perform ? shim_perform : 0;
}
