/*
 * WynlandOS - the screen's back-buffer
 * ============================================================
 * Contiguous memory the size of the screen. virtio-gpu scans it out (it
 * is resource 1's backing memory, drivers/video/virtio_gpu.c), and
 * /dev/fb0 maps it while virtio-gpu is up; without virtio-gpu the GOP
 * framebuffer is the screen.
 *
 * (What remains of the kernel's own desktop, gui/compositor.c: the
 * desktop is userspace's -- Zerp.)
 */

#include <wynland/types.h>
#include <wynland/boot_info.h>
#include <wynland/vmm.h>
#include <wynland/pmm.h>

extern void serial_write_string(const char *str);

static BootInfo *g_info;
static uint32_t *g_back;
static uint64_t  g_back_phys;
static uint32_t  g_width, g_height;

void compositor_init(BootInfo *info)
{
    g_info = info;
    g_width = info->fb_width;
    g_height = info->fb_height;

    uint64_t pages = ((uint64_t)g_width * g_height * 4 + 4095) / 4096;
    uint8_t *phys = (uint8_t *)pmm_alloc_contiguous(pages);
    if (!phys) {
        serial_write_string("backbuffer: no contiguous memory for the screen\r\n");
        return;
    }
    const uint64_t virt = 0x90000000ULL;
    PageTable *pml4 = vmm_get_current_pml4();
    for (uint64_t i = 0; i < pages; i++)
        vmm_map_page(pml4, virt + i * 4096, (uint64_t)(uintptr_t)phys + i * 4096, PAGE_WRITE | PAGE_NX);
    g_back = (uint32_t *)(uintptr_t)virt;
    g_back_phys = (uint64_t)(uintptr_t)phys;

    for (uint64_t i = 0; i < (uint64_t)g_width * g_height; i++) g_back[i] = 0x002E3440;
}

uint32_t *comp_get_backbuffer(void) { return g_back; }
uint32_t  comp_get_width(void)      { return g_width; }
uint32_t  comp_get_height(void)     { return g_height; }

/* Whichever physical memory the display is scanned from right now:
   virtio-gpu reads the back-buffer, never the GOP framebuffer, so
   anything writing to the screen (/dev/fb0) targets this address. */
uint64_t fb_active_phys_addr(void)
{
    extern bool virtio_gpu_is_active(void);
    return virtio_gpu_is_active() ? g_back_phys : g_info->fb_addr;
}
