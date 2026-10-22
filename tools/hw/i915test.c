/*
 * WynlandOS - i915test: the kernel's i915 interface (drivers/gpu/i915_uapi.c)
 * used the way Mesa's iris uses it, without Mesa: the device's facts, an
 * object written by a batch through softpinned addresses, read back
 * through its mapping. Built on the developer's machine against Linux's
 * own i915_drm.h: gcc -O2 -static.
 */
#include <drm/i915_drm.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

static int fd;

static int io(unsigned long req, void *arg, const char *what)
{
    int r = ioctl(fd, req, arg);
    if (r) printf("%-28s FAILED: %s\n", what, strerror(errno));
    return r;
}

static int param(int p, const char *what)
{
    int v = -1;
    struct drm_i915_getparam gp = { p, &v };
    if (io(DRM_IOCTL_I915_GETPARAM, &gp, what) == 0) printf("%-28s %d (0x%x)\n", what, v, v);
    return v;
}

int main(void)
{
    fd = open("/dev/dri/renderD128", O_RDWR);
    if (fd < 0) { perror("/dev/dri/renderD128"); return 1; }

    char name[32] = { 0 }, desc[64] = { 0 };
    struct drm_version ver = { 0 };
    ver.name = name; ver.name_len = sizeof name - 1; ver.desc = desc; ver.desc_len = sizeof desc - 1;
    if (io(DRM_IOCTL_VERSION, &ver, "VERSION")) return 1;
    printf("driver: %s (%s), %d.%d\n", name, desc, ver.version_major, ver.version_minor);
    if (strcmp(name, "i915")) { printf("not i915 (boot.cfg gpu=1?): stopping\n"); return 1; }

    param(I915_PARAM_CHIPSET_ID, "CHIPSET_ID");
    param(I915_PARAM_REVISION, "REVISION");
    param(I915_PARAM_CS_TIMESTAMP_FREQUENCY, "CS_TIMESTAMP_FREQUENCY");
    param(I915_PARAM_EU_TOTAL, "EU_TOTAL");
    param(I915_PARAM_SUBSLICE_TOTAL, "SUBSLICE_TOTAL");
    param(I915_PARAM_HAS_EXEC_SOFTPIN, "HAS_EXEC_SOFTPIN");

    /* QUERY: engines */
    struct drm_i915_query_item item = { .query_id = DRM_I915_QUERY_ENGINE_INFO };
    struct drm_i915_query q = { 1, 0, (uintptr_t)&item };
    if (!io(DRM_IOCTL_I915_QUERY, &q, "QUERY engines (size)") && item.length > 0) {
        struct drm_i915_query_engine_info *ei = calloc(1, item.length);
        item.data_ptr = (uintptr_t)ei;
        if (!io(DRM_IOCTL_I915_QUERY, &q, "QUERY engines") && item.length > 0)
            for (unsigned k = 0; k < ei->num_engines; k++)
                printf("engine %u: class %u instance %u\n", k, ei->engines[k].engine.engine_class, ei->engines[k].engine.engine_instance);
    }

    /* two objects: a batch and a target */
    struct drm_i915_gem_create cb = { 4096 }, ct = { 4096 };
    if (io(DRM_IOCTL_I915_GEM_CREATE, &cb, "GEM_CREATE batch") || io(DRM_IOCTL_I915_GEM_CREATE, &ct, "GEM_CREATE target")) return 1;
    struct drm_i915_gem_mmap_offset mb = { .handle = cb.handle, .flags = I915_MMAP_OFFSET_WB }, mt = { .handle = ct.handle, .flags = I915_MMAP_OFFSET_WB };
    if (io(DRM_IOCTL_I915_GEM_MMAP_OFFSET, &mb, "MMAP_OFFSET batch") || io(DRM_IOCTL_I915_GEM_MMAP_OFFSET, &mt, "MMAP_OFFSET target")) return 1;
    uint32_t *batch = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, mb.offset);
    uint32_t *target = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, mt.offset);
    if (batch == MAP_FAILED || target == MAP_FAILED) { perror("mmap"); return 1; }

    /* a context with an engine map: [render, copy], as iris makes them */
    struct { uint64_t extensions; struct i915_engine_class_instance e[2]; } engines = { 0, { { 0, 0 }, { 1, 0 } } };
    struct drm_i915_gem_context_create_ext_setparam sp = {
        .base = { .name = I915_CONTEXT_CREATE_EXT_SETPARAM },
        .param = { .param = I915_CONTEXT_PARAM_ENGINES, .size = sizeof engines, .value = (uintptr_t)&engines } };
    struct drm_i915_gem_context_create_ext cc = { .flags = I915_CONTEXT_CREATE_FLAGS_USE_EXTENSIONS, .extensions = (uintptr_t)&sp };
    if (io(DRM_IOCTL_I915_GEM_CONTEXT_CREATE_EXT, &cc, "CONTEXT_CREATE_EXT")) return 1;
    printf("context %u\n", cc.ctx_id);

    int ok = 1;
    for (int eng = 0; eng < 2; eng++) {
        uint64_t va_batch = 0x200000000ull, va_target = 0x200100000ull;
        uint32_t want = 0x1915F00Du + eng;
        int i = 0;
        batch[i++] = (0x20u << 23) | 2;                 /* MI_STORE_DATA_IMM through the PPGTT */
        batch[i++] = (uint32_t)va_target + 4 * eng;
        batch[i++] = (uint32_t)(va_target >> 32);
        batch[i++] = want;
        batch[i++] = 0x0A << 23;                        /* MI_BATCH_BUFFER_END */
        batch[i++] = 0;
        struct drm_i915_gem_exec_object2 obj[2] = {
            { .handle = cb.handle, .offset = va_batch, .flags = EXEC_OBJECT_PINNED | EXEC_OBJECT_SUPPORTS_48B_ADDRESS },
            { .handle = ct.handle, .offset = va_target, .flags = EXEC_OBJECT_PINNED | EXEC_OBJECT_SUPPORTS_48B_ADDRESS | EXEC_OBJECT_WRITE } };
        struct drm_i915_gem_execbuffer2 eb = {
            .buffers_ptr = (uintptr_t)obj, .buffer_count = 2, .batch_len = i * 4,
            .flags = (uint64_t)eng | I915_EXEC_NO_RELOC | I915_EXEC_HANDLE_LUT | I915_EXEC_BATCH_FIRST,
            .rsvd1 = cc.ctx_id };
        if (io(DRM_IOCTL_I915_GEM_EXECBUFFER2, &eb, eng ? "EXECBUFFER2 copy engine" : "EXECBUFFER2 render engine")) { ok = 0; continue; }
        struct drm_i915_gem_wait w = { .bo_handle = ct.handle, .timeout_ns = 1000000000 };
        io(DRM_IOCTL_I915_GEM_WAIT, &w, "GEM_WAIT");
        printf("%s engine: target[%d] = %08x %s\n", eng ? "copy" : "render", eng, target[eng], target[eng] == want ? "(the batch wrote it)" : "WRONG");
        if (target[eng] != want) ok = 0;
    }
    printf(ok ? "I915 INTERFACE WORKS: a batch ran through it on both engines\n" : "i915test: something failed\n");
    return ok ? 0 : 2;
}
