/*
 * WynlandOS - vktest: Vulkan on the Intel GPU (anv) through the kernel's
 * i915 interface, with no window: a buffer filled (vkCmdFillBuffer), an
 * image cleared and a triangle drawn into it with a real pipeline (SPIR-V
 * shaders: tools/hw/vktest.vert / .frag, built by glslangValidator into
 * vktest_spv.h), the image copied to a buffer and its pixels checked.
 *
 * gcc -O2 -I<vulkan headers> -o vktest vktest.c -lvulkan
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>
#include "vktest_spv.h"

#define W 64
#define H 64
#define CK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { printf("FAIL: %s = %d\n", #x, r_); return 1; } } while (0)

static VkPhysicalDeviceMemoryProperties g_mem;

static uint32_t mem_type(uint32_t bits, VkMemoryPropertyFlags want)
{
    for (uint32_t i = 0; i < g_mem.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (g_mem.memoryTypes[i].propertyFlags & want) == want) return i;
    return 0;
}

static VkDeviceMemory bind_mem(VkDevice dev, VkMemoryRequirements req, VkMemoryPropertyFlags want)
{
    VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, req.size, mem_type(req.memoryTypeBits, want) };
    VkDeviceMemory m = VK_NULL_HANDLE;
    if (vkAllocateMemory(dev, &ai, NULL, &m) != VK_SUCCESS) printf("FAIL: vkAllocateMemory\n");
    return m;
}

int main(void)
{
    VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "vktest", 1, "wynland", 1, VK_API_VERSION_1_3 };
    VkInstanceCreateInfo ici = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0, &app, 0, NULL, 0, NULL };
    VkInstance inst;
    CK(vkCreateInstance(&ici, NULL, &inst));
    uint32_t n = 1;
    VkPhysicalDevice pd;
    CK(vkEnumeratePhysicalDevices(inst, &n, &pd));
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(pd, &props);
    printf("device: %s (Vulkan %u.%u)\n", props.deviceName, VK_API_VERSION_MAJOR(props.apiVersion), VK_API_VERSION_MINOR(props.apiVersion));
    vkGetPhysicalDeviceMemoryProperties(pd, &g_mem);

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, NULL, 0, 0, 1, &prio };
    VkDeviceCreateInfo dci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, NULL, 0, 1, &qci, 0, NULL, 0, NULL, NULL };
    VkDevice dev;
    CK(vkCreateDevice(pd, &dci, NULL, &dev));
    VkQueue q;
    vkGetDeviceQueue(dev, 0, 0, &q);
    printf("device and queue made\n");

    /* the colour image, and a host-visible buffer for its pixels and one more to fill */
    VkImageCreateInfo imi = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, NULL, 0, VK_IMAGE_TYPE_2D, VK_FORMAT_R8G8B8A8_UNORM,
        { W, H, 1 }, 1, 1, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_TILING_OPTIMAL,
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        VK_SHARING_MODE_EXCLUSIVE, 0, NULL, VK_IMAGE_LAYOUT_UNDEFINED };
    VkImage img;
    CK(vkCreateImage(dev, &imi, NULL, &img));
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(dev, img, &req);
    VkDeviceMemory imem = bind_mem(dev, req, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    CK(vkBindImageMemory(dev, img, imem, 0));

    VkBuffer buf[2];
    VkDeviceMemory bmem[2];
    void *bmap[2];
    for (int k = 0; k < 2; k++) {
        VkBufferCreateInfo bi = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, W * H * 4,
            VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_SHARING_MODE_EXCLUSIVE, 0, NULL };
        CK(vkCreateBuffer(dev, &bi, NULL, &buf[k]));
        vkGetBufferMemoryRequirements(dev, buf[k], &req);
        bmem[k] = bind_mem(dev, req, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        CK(vkBindBufferMemory(dev, buf[k], bmem[k], 0));
        CK(vkMapMemory(dev, bmem[k], 0, W * H * 4, 0, &bmap[k]));
        memset(bmap[k], 0, W * H * 4);
    }

    /* the pipeline: a render pass, the image as its colour attachment */
    VkAttachmentDescription att = { 0, VK_FORMAT_R8G8B8A8_UNORM, VK_SAMPLE_COUNT_1_BIT, VK_ATTACHMENT_LOAD_OP_CLEAR,
        VK_ATTACHMENT_STORE_OP_STORE, VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_STORE_OP_DONT_CARE,
        VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL };
    VkAttachmentReference ref = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    VkSubpassDescription sub = { 0, VK_PIPELINE_BIND_POINT_GRAPHICS, 0, NULL, 1, &ref, NULL, NULL, 0, NULL };
    VkRenderPassCreateInfo rpi = { VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, NULL, 0, 1, &att, 1, &sub, 0, NULL };
    VkRenderPass rp;
    CK(vkCreateRenderPass(dev, &rpi, NULL, &rp));
    VkImageViewCreateInfo vi = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, NULL, 0, img, VK_IMAGE_VIEW_TYPE_2D,
        VK_FORMAT_R8G8B8A8_UNORM, { 0 }, { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
    VkImageView view;
    CK(vkCreateImageView(dev, &vi, NULL, &view));
    VkFramebufferCreateInfo fbi = { VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, NULL, 0, rp, 1, &view, W, H, 1 };
    VkFramebuffer fb;
    CK(vkCreateFramebuffer(dev, &fbi, NULL, &fb));

    VkShaderModuleCreateInfo smi[2] = {
        { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0, sizeof vert_spv, vert_spv },
        { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0, sizeof frag_spv, frag_spv } };
    VkShaderModule sm[2];
    CK(vkCreateShaderModule(dev, &smi[0], NULL, &sm[0]));
    CK(vkCreateShaderModule(dev, &smi[1], NULL, &sm[1]));
    VkPipelineShaderStageCreateInfo st[2] = {
        { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_VERTEX_BIT, sm[0], "main", NULL },
        { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_FRAGMENT_BIT, sm[1], "main", NULL } };
    VkPipelineVertexInputStateCreateInfo vin = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    VkPipelineInputAssemblyStateCreateInfo ia = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, NULL, 0,
        VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, VK_FALSE };
    VkViewport vp = { 0, 0, W, H, 0, 1 };
    VkRect2D sc = { { 0, 0 }, { W, H } };
    VkPipelineViewportStateCreateInfo vps = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, NULL, 0, 1, &vp, 1, &sc };
    VkPipelineRasterizationStateCreateInfo rs = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO, NULL, 0,
        VK_FALSE, VK_FALSE, VK_POLYGON_MODE_FILL, VK_CULL_MODE_NONE, VK_FRONT_FACE_COUNTER_CLOCKWISE, VK_FALSE, 0, 0, 0, 1.0f };
    VkPipelineMultisampleStateCreateInfo ms = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, NULL, 0,
        VK_SAMPLE_COUNT_1_BIT };
    VkPipelineColorBlendAttachmentState cba = { VK_FALSE, 0, 0, 0, 0, 0, 0, 0xF };
    VkPipelineColorBlendStateCreateInfo cb = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, NULL, 0,
        VK_FALSE, 0, 1, &cba };
    VkPipelineLayoutCreateInfo pli = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    VkPipelineLayout pl;
    CK(vkCreatePipelineLayout(dev, &pli, NULL, &pl));
    VkGraphicsPipelineCreateInfo gpi = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, NULL, 0, 2, st, &vin, &ia, NULL,
        &vps, &rs, &ms, NULL, &cb, NULL, pl, rp, 0, VK_NULL_HANDLE, 0 };
    VkPipeline pipe;
    CK(vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gpi, NULL, &pipe));
    printf("pipeline made (shaders compiled by anv)\n");

    /* the commands */
    VkCommandPoolCreateInfo cpi = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL, 0, 0 };
    VkCommandPool pool;
    CK(vkCreateCommandPool(dev, &cpi, NULL, &pool));
    VkCommandBufferAllocateInfo cai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1 };
    VkCommandBuffer cmd;
    CK(vkAllocateCommandBuffers(dev, &cai, &cmd));
    VkCommandBufferBeginInfo cbi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    CK(vkBeginCommandBuffer(cmd, &cbi));
    vkCmdFillBuffer(cmd, buf[1], 0, W * H * 4, 0xC0FFEE11u);
    VkClearValue clear = { .color = { { 0.2f, 0.4f, 0.8f, 1.0f } } };
    VkRenderPassBeginInfo rbi = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, NULL, rp, fb, { { 0, 0 }, { W, H } }, 1, &clear };
    vkCmdBeginRenderPass(cmd, &rbi, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRenderPass(cmd);
    VkBufferImageCopy cp = { 0, 0, 0, { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, { 0, 0, 0 }, { W, H, 1 } };
    vkCmdCopyImageToBuffer(cmd, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf[0], 1, &cp);
    VkMemoryBarrier mb = { VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT };
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
    CK(vkEndCommandBuffer(cmd));

    VkFenceCreateInfo fci = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    VkFence fence;
    CK(vkCreateFence(dev, &fci, NULL, &fence));
    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL, 0, NULL, NULL, 1, &cmd, 0, NULL };
    CK(vkQueueSubmit(q, 1, &si, fence));
    CK(vkWaitForFences(dev, 1, &fence, VK_TRUE, 3000000000ull));
    printf("submitted and waited for\n");

    uint32_t *fill = bmap[1];
    unsigned char *px = bmap[0];
    unsigned char *in = &px[((H - 5) * W + 4) * 4], *out = &px[(4 * W + (W - 5)) * 4];
    printf("fill: %08x  inside=(%u,%u,%u,%u) outside=(%u,%u,%u,%u)\n", fill[100],
           in[0], in[1], in[2], in[3], out[0], out[1], out[2], out[3]);
    int ok = fill[100] == 0xC0FFEE11u && in[0] > 240 && in[1] < 15 && in[2] < 15 &&
             out[0] > 40 && out[0] < 62 && out[1] > 92 && out[1] < 112 && out[2] > 194 && out[2] < 214;
    printf(ok ? "PASS: VULKAN DREW ON THE INTEL GPU\n" : "FAIL: unexpected data\n");
    vkDeviceWaitIdle(dev);
    return ok ? 0 : 2;
}
