/* The GPU half of gralloc.default (gralloc_shim.c): with the guest
 * rendering through gfxstream, a buffer is a host ColorBuffer that
 * vine_kumquat allocates on its gralloc socket, plus a shared CPU copy
 * for locks. The handle is gfxstream's goldfish cb_handle_t, which
 * gfxstream-vk (and so ANGLE) imports by its host handle.
 *
 * CPU access copies through the host: a read lock fetches the
 * ColorBuffer into the shared memory, a write unlock uploads it. The
 * second fd of a handle keeps the ColorBuffer alive; vine_kumquat frees
 * it when every process has closed its copy. */
#include "gralloc_gpu.h"

#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#define SOCKET_PATH "/run/user/1000/vine-gralloc"

/* hardware/graphics.h */
#define FMT_RGBA_8888 1
#define FMT_RGBX_8888 2
#define FMT_RGB_888 3
#define FMT_RGB_565 4
#define FMT_BGRA_8888 5
#define FMT_RGBA_FP16 0x16
#define FMT_BLOB 0x21
#define FMT_IMPLEMENTATION_DEFINED 0x22
#define FMT_YCBCR_420_888 0x23
#define FMT_RGBA_1010102 0x2b
#define FMT_R_8 0x38
#define FMT_YV12 0x32315659

/* gralloc usage */
#define USAGE_SW_READ_MASK 0xfu
#define USAGE_SW_WRITE_MASK 0xf0u
#define USAGE_HW_COMPOSER 0x800u
#define USAGE_HW_FB 0x1000u

/* virgl formats and binds, as gfxstream reads them */
#define VIRGL_B8G8R8A8 1
#define VIRGL_B5G6R5 7
#define VIRGL_R8 64
#define VIRGL_R8G8B8A8 67
#define VIRGL_R16G16B16A16_FLOAT 94
#define VIRGL_R10G10B10A2 8
#define VIRGL_R8G8B8X8 134
#define VIRGL_YV12 163
#define VIRGL_NV12 166
#define BIND_RENDER_TARGET (1 << 1)
#define BIND_SAMPLER_VIEW (1 << 3)
#define BIND_LINEAR (1 << 22)
#define BIND_SCANOUT (1 << 18)
#define PIPE_BUFFER 0
#define PIPE_TEXTURE_2D 2

#define FOURCC(a, b, c, d) ((uint32_t)(a) | ((uint32_t)(b) << 8) | ((uint32_t)(c) << 16) | ((uint32_t)(d) << 24))

/* gralloc_cb_bp.h's cb_handle_t, laid out by hand */
#define CB_MAGIC 0xABFABF05u
typedef struct {
    int version, numFds, numInts;
    int32_t bufferFd;
    int32_t refcountFd;
    uint32_t pad;
    uint64_t usage;
    uint64_t mmapedOffset; /* here: width | height << 32 */
    uint64_t bufferPtr64;
    uint32_t magic;
    uint32_t hostHandle;
    uint32_t format;
    uint32_t drmformat;
    uint32_t mmapedSize;
    uint32_t bufferSize;
    uint32_t externalMetadataOffset;
    uint32_t stride; /* pixels */
    uint32_t lockState; /* here: the usage of the current lock */
    uint32_t pad2;
} cb_handle;
_Static_assert(sizeof(cb_handle) == 88, "cb_handle_t is 88 bytes");

struct format_info {
    uint32_t virgl, drm, bpp; /* bpp in bytes; 0 for planar YUV */
};

static int format_info(int format, struct format_info* out) {
    switch (format) {
    case FMT_RGBA_8888: *out = (struct format_info){VIRGL_R8G8B8A8, FOURCC('A', 'B', '2', '4'), 4}; return 0;
    case FMT_RGBX_8888: *out = (struct format_info){VIRGL_R8G8B8X8, FOURCC('X', 'B', '2', '4'), 4}; return 0;
    case FMT_BGRA_8888: *out = (struct format_info){VIRGL_B8G8R8A8, FOURCC('A', 'R', '2', '4'), 4}; return 0;
    case FMT_RGB_565: *out = (struct format_info){VIRGL_B5G6R5, FOURCC('R', 'G', '1', '6'), 2}; return 0;
    case FMT_RGBA_FP16: *out = (struct format_info){VIRGL_R16G16B16A16_FLOAT, FOURCC('A', 'B', '4', 'H'), 8}; return 0;
    case FMT_RGBA_1010102: *out = (struct format_info){VIRGL_R10G10B10A2, FOURCC('A', 'B', '3', '0'), 4}; return 0;
    case FMT_R_8: *out = (struct format_info){VIRGL_R8, FOURCC('R', '8', ' ', ' '), 1}; return 0;
    case FMT_YV12: *out = (struct format_info){VIRGL_YV12, FOURCC('Y', 'V', '1', '2'), 0}; return 0;
    case FMT_YCBCR_420_888: *out = (struct format_info){VIRGL_NV12, FOURCC('N', 'V', '1', '2'), 0}; return 0;
    case FMT_BLOB: *out = (struct format_info){VIRGL_R8, 0, 1}; return 0;
    }
    return -1;
}

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_sock = -1;

static int service_fd(void) {
    if (g_sock >= 0) return g_sock;
    int fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    struct sockaddr_un sa = {.sun_family = AF_UNIX};
    strcpy(sa.sun_path, SOCKET_PATH);
    if (fd < 0 || connect(fd, (struct sockaddr*)&sa, sizeof sa) != 0) {
        if (fd >= 0) close(fd);
        return -1;
    }
    return g_sock = fd;
}

/* One request: 12 words out, (status, value) back with up to 2 fds. */
static int request(const uint32_t req[12], uint32_t* value, int fds[2]) {
    pthread_mutex_lock(&g_lock);
    int r = -EIO;
    int fd = service_fd();
    if (fd < 0) goto out;
    if (send(fd, req, 48, MSG_NOSIGNAL) != 48) {
        close(fd);
        g_sock = -1;
        goto out;
    }
    int32_t reply[2];
    struct iovec iov = {reply, sizeof reply};
    char control[CMSG_SPACE(2 * sizeof(int))];
    struct msghdr mh = {.msg_iov = &iov, .msg_iovlen = 1, .msg_control = control, .msg_controllen = sizeof control};
    ssize_t n = recvmsg(fd, &mh, MSG_CMSG_CLOEXEC);
    if (n != sizeof reply) {
        close(fd);
        g_sock = -1;
        goto out;
    }
    int got = 0;
    for (struct cmsghdr* c = CMSG_FIRSTHDR(&mh); c; c = CMSG_NXTHDR(&mh, c)) {
        if (c->cmsg_level != SOL_SOCKET || c->cmsg_type != SCM_RIGHTS) continue;
        int k = (int)((c->cmsg_len - CMSG_LEN(0)) / sizeof(int));
        for (int i = 0; i < k; ++i) {
            int f;
            memcpy(&f, CMSG_DATA(c) + i * sizeof(int), sizeof f);
            if (fds && got < 2) fds[got++] = f;
            else close(f);
        }
    }
    r = reply[0];
    if (value) *value = (uint32_t)reply[1];
    if (r == 0 && fds && got < 2) r = -EIO;
out:
    pthread_mutex_unlock(&g_lock);
    return r;
}

static cb_handle* from(const void* h) {
    cb_handle* cb = (cb_handle*)h;
    if (!cb || cb->version != 12 || cb->numFds != 2 || cb->numInts != 17 || cb->magic != CB_MAGIC) return NULL;
    return cb;
}

static uint32_t width_of(const cb_handle* cb) { return (uint32_t)cb->mmapedOffset; }
static uint32_t height_of(const cb_handle* cb) { return (uint32_t)(cb->mmapedOffset >> 32); }

int vine_gpu_alloc(int w, int h, int format, int usage, const void** handle, int* stride) {
    if (w <= 0 || h <= 0) return -EINVAL;
    if (format == FMT_IMPLEMENTATION_DEFINED) format = FMT_RGBA_8888;
    if (format == FMT_RGB_888) format = FMT_RGBX_8888;
    struct format_info fi;
    if (format_info(format, &fi)) return -EINVAL;
    uint32_t s, size, target = PIPE_TEXTURE_2D, bind = BIND_RENDER_TARGET | BIND_SAMPLER_VIEW, width = w, height = h;
    if (format == FMT_BLOB) {
        target = PIPE_BUFFER;
        bind = BIND_LINEAR;
        s = w;
        size = w * h;
        width = w * h;
        height = 1;
    } else if (!fi.bpp) {
        s = (w + 15) & ~15u;
        size = s * h + 2 * (((s / 2 + 15) & ~15u) * ((h + 1) / 2));
    } else {
        /* gfxstream transfers ColorBuffers tightly packed, whatever
         * stride the request names */
        s = w;
        size = s * h * fi.bpp;
    }
    if (usage & (USAGE_HW_COMPOSER | USAGE_HW_FB)) bind |= BIND_SCANOUT;
    size = (size + 4095) & ~4095u;
    uint32_t req[12] = {1, target, fi.virgl, bind, width, height, size};
    uint32_t id = 0;
    int fds[2] = {-1, -1};
    int r = request(req, &id, fds);
    if (r) return r;
    cb_handle* cb = calloc(1, sizeof *cb);
    if (!cb) {
        close(fds[0]);
        close(fds[1]);
        return -ENOMEM;
    }
    cb->version = 12;
    cb->numFds = 2;
    cb->numInts = 17;
    cb->bufferFd = fds[0];
    cb->refcountFd = fds[1];
    cb->usage = (uint32_t)usage;
    cb->mmapedOffset = (uint64_t)w | (uint64_t)h << 32;
    cb->magic = CB_MAGIC;
    cb->hostHandle = id;
    cb->format = format;
    cb->drmformat = fi.drm;
    cb->mmapedSize = size;
    cb->bufferSize = size;
    cb->stride = s;
    *handle = cb;
    *stride = (int)s;
    return 0;
}

static void unmap(cb_handle* cb) {
    if (cb->bufferPtr64) munmap((void*)(uintptr_t)cb->bufferPtr64, cb->bufferSize);
    cb->bufferPtr64 = 0;
}

int vine_gpu_free(const void* handle) {
    cb_handle* cb = from(handle);
    if (!cb) return -EINVAL;
    unmap(cb);
    close(cb->bufferFd);
    close(cb->refcountFd);
    free(cb);
    return 0;
}

int vine_gpu_register(const void* handle) {
    cb_handle* cb = from(handle);
    if (!cb) return -EINVAL;
    cb->bufferPtr64 = 0; /* a mapping of the sender's */
    cb->lockState = 0;
    return 0;
}

int vine_gpu_unregister(const void* handle) {
    cb_handle* cb = from(handle);
    if (!cb) return -EINVAL;
    unmap(cb);
    return 0;
}

static int transfer(cb_handle* cb, int to_host) {
    if (cb->format == FMT_BLOB) {
        uint32_t req[12] = {2, cb->hostHandle, (uint32_t)to_host, 0, 0, width_of(cb) * height_of(cb), 1, 0};
        return request(req, NULL, NULL);
    }
    struct format_info fi;
    format_info(cb->format, &fi);
    uint32_t req[12] = {2, cb->hostHandle, (uint32_t)to_host, 0, 0, width_of(cb), height_of(cb),
                        fi.bpp ? cb->stride * fi.bpp : cb->stride};
    return request(req, NULL, NULL);
}

int vine_gpu_lock(const void* handle, int usage, void** vaddr) {
    cb_handle* cb = from(handle);
    if (!cb) return -EINVAL;
    if (!cb->bufferPtr64) {
        void* p = mmap(NULL, cb->bufferSize, PROT_READ | PROT_WRITE, MAP_SHARED, cb->bufferFd, 0);
        if (p == MAP_FAILED) return -errno;
        cb->bufferPtr64 = (uintptr_t)p;
    }
    /* The host may hold newer pixels than the shared copy */
    if ((usage & USAGE_SW_READ_MASK) && transfer(cb, 0)) return -EIO;
    cb->lockState = (uint32_t)usage;
    *vaddr = (void*)(uintptr_t)cb->bufferPtr64;
    return 0;
}

int vine_gpu_lock_ycbcr(const void* handle, int usage, struct vine_ycbcr* out) {
    cb_handle* cb = from(handle);
    if (!cb) return -EINVAL;
    void* p;
    int r = vine_gpu_lock(handle, usage, &p);
    if (r) return r;
    uint8_t* y = p;
    uint32_t h = height_of(cb), s = cb->stride;
    memset(out, 0, sizeof *out);
    out->y = y;
    out->ystride = s;
    if (cb->format == FMT_YV12) {
        uint32_t cs = (s / 2 + 15) & ~15u;
        out->cr = y + s * h;
        out->cb = y + s * h + cs * ((h + 1) / 2);
        out->cstride = cs;
        out->chroma_step = 1;
    } else {
        out->cb = y + s * h;
        out->cr = y + s * h + 1;
        out->cstride = s;
        out->chroma_step = 2;
    }
    return 0;
}

int vine_gpu_unlock(const void* handle) {
    cb_handle* cb = from(handle);
    if (!cb) return -EINVAL;
    int r = 0;
    if (cb->lockState & USAGE_SW_WRITE_MASK) r = transfer(cb, 1) ? -EIO : 0;
    cb->lockState = 0;
    return r;
}
