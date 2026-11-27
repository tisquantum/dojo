/* The Vulkan renderer (vk_renderer.h), Vulkan 1.3 with dynamic rendering.
 *
 * Surfaces. Primitives are drawn into surfaces at `scale` times the console's resolution: one for all of
 * video memory, and one for each of the game's two display buffers (640x240 at y 0 and 240), which a
 * primitive goes to when its drawing area lies inside one. Every surface is RGBA8 and stays in the GENERAL
 * layout, so it can be drawn into, sampled and copied without layout changes.
 *
 * Widescreen. A display buffer's surface is wider than the console's, by a margin each side: the 3D scene
 * projects into the margins as it would past the console's screen edges (the drawers' and the stage
 * cull's edge tests are widened to match, tools/mips2c.py and src/stage_cull.c), a drawing area that is the
 * whole buffer extends over them, and so does an untextured primitive with corners exactly on the
 * buffer's left and right edges (the full-screen fades, tints and backdrops) and a fill of the whole
 * width (the background clear). Textured full-screen pictures keep their 4:3 width, pillarboxed.
 *
 * Video memory. The command processor's g_vram (gpu.c) holds what the game uploads, fills and copies; it
 * is mirrored at 1x in a 16-bit integer image the draw shader reads textures and colour tables from, and
 * painted into the surfaces it overlaps, so what the game loads straight into a frame buffer shows. What the
 * renderer draws exists only in the surfaces until the game reads it back or samples it as a texture: each
 * surface keeps the box it has drawn into, and such reads bring that box down to g_vram first.
 *
 * Display. The shown picture goes to the window in one of the display modes (present.frag): as drawn,
 * smoothed (supersampled down to the window), or from a copy brought down to the console's resolution
 * (downsample.frag) as sharp pixels or one of the CRT looks.
 *
 * Frames. Commands are recorded into one command buffer between presents; uploads and readbacks interrupt
 * the rendering for transfers, behind full barriers. A present, and any readback, submits and waits, so the
 * vertex and upload rings start empty after each. The workload is a console's: waiting costs nothing. */

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <SDL3/SDL_vulkan.h>
#include <stdio.h>
#include <string.h>
#include "gpu.h"
#include "gpu_renderer.h"
#include "host.h"
#include "vk_renderer.h"
#include "overlay.h"

#include "shaders/draw_vert.h"
#include "shaders/draw_frag.h"
#include "shaders/quad_vert.h"
#include "shaders/paint_frag.h"
#include "shaders/downsample_frag.h"
#include "shaders/present_frag.h"
#include "shaders/overlay_frag.h"

/* Vulkan entry points, loaded through SDL's vkGetInstanceProcAddr. */
#define VK_INSTANCE_FUNCTIONS(X)                                                                           \
    X(vkDestroyInstance) X(vkEnumeratePhysicalDevices) X(vkGetPhysicalDeviceProperties)                    \
    X(vkGetPhysicalDeviceQueueFamilyProperties) X(vkGetPhysicalDeviceMemoryProperties) X(vkCreateDevice)   \
    X(vkGetDeviceProcAddr) X(vkGetPhysicalDeviceSurfaceSupportKHR) X(vkGetPhysicalDeviceSurfaceCapabilitiesKHR) \
    X(vkGetPhysicalDeviceSurfaceFormatsKHR) X(vkGetPhysicalDeviceSurfacePresentModesKHR) X(vkDestroySurfaceKHR)  \
    X(vkEnumerateDeviceExtensionProperties)
#define VK_DEVICE_FUNCTIONS(X)                                                                             \
    X(vkDestroyDevice) X(vkGetDeviceQueue) X(vkCreateSwapchainKHR) X(vkDestroySwapchainKHR)                \
    X(vkGetSwapchainImagesKHR) X(vkAcquireNextImageKHR) X(vkQueuePresentKHR) X(vkCreateImageView)          \
    X(vkDestroyImageView) X(vkCreateImage) X(vkGetImageMemoryRequirements) X(vkAllocateMemory)             \
    X(vkBindImageMemory) X(vkCreateBuffer) X(vkGetBufferMemoryRequirements) X(vkBindBufferMemory)          \
    X(vkMapMemory) X(vkCreateSampler) X(vkCreateDescriptorSetLayout) X(vkCreatePipelineLayout)             \
    X(vkCreateShaderModule) X(vkDestroyShaderModule) X(vkCreateGraphicsPipelines) X(vkCreateDescriptorPool) \
    X(vkAllocateDescriptorSets) X(vkUpdateDescriptorSets) X(vkCreateCommandPool) X(vkAllocateCommandBuffers) \
    X(vkBeginCommandBuffer) X(vkEndCommandBuffer) X(vkQueueSubmit) X(vkDeviceWaitIdle) X(vkCreateFence)    \
    X(vkWaitForFences) X(vkResetFences) X(vkCreateSemaphore) X(vkCmdBeginRendering) X(vkCmdEndRendering)   \
    X(vkCmdPipelineBarrier2) X(vkCmdBindPipeline) X(vkCmdBindDescriptorSets) X(vkCmdBindVertexBuffers)     \
    X(vkCmdPushConstants) X(vkCmdSetViewport) X(vkCmdSetScissor) X(vkCmdSetBlendConstants) X(vkCmdDraw)     \
    X(vkCmdCopyBufferToImage) X(vkCmdCopyImageToBuffer) X(vkCmdClearColorImage) X(vkCmdClearAttachments)  \
    X(vkDestroyImage) X(vkFreeMemory) X(vkDestroyBuffer)

#define DECLARE(name) static PFN_##name name;
static PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr;
static PFN_vkCreateInstance vkCreateInstance;
VK_INSTANCE_FUNCTIONS(DECLARE)
VK_DEVICE_FUNCTIONS(DECLARE)
#undef DECLARE

#define SURFACE_VRAM 0
#define SURFACE_FB0  1
#define SURFACE_FB1  2
#define SURFACE_COUNT 3

#define VERTEX_RING_BYTES  (16u << 20)
#define UPLOAD_RING_BYTES  (16u << 20)
#define MAX_SWAPCHAIN_IMAGES 8

enum {
    PIPE_OPAQUE, PIPE_SEMI0, PIPE_SEMI1, PIPE_SEMI2, PIPE_SEMI3, PIPE_PAINT, PIPE_DOWNSAMPLE, PIPE_PRESENT,
    PIPE_SHOWN, PIPE_OVERLAY, PIPE_COUNT
};

/* present.frag's display modes. */
enum { DISPLAY_BILINEAR, DISPLAY_SMOOTH, DISPLAY_SHARP, DISPLAY_SCANLINES, DISPLAY_APERTURE, DISPLAY_MASK };

/* The picture at the console's resolution: the widest display with the widest margins, and 480 lines. */
#define NATIVE_WIDTH  (VRAM_WIDTH + 2 * 640)
#define NATIVE_HEIGHT 480

typedef struct Vertex {
    float x, y;
    uint8_t r, g, b, a;
    float u, v;
    uint32_t texture[4];
} Vertex;

typedef struct Push {
    float rect[4];
    float extra[4];
    float params[4]; /* present.frag's: the display mode, the curvature, the picture's size in pixels */
} Push;

typedef struct Image {
    VkImage image;
    VkImageView view;
    VkDeviceMemory memory;
    int width, height; /* in pixels */
} Image;

typedef struct Surface {
    int x, y, w, h; /* the part of video memory it holds, in console pixels */
    int margin;     /* widescreen: its extra width each side, in console pixels */
    Image image;
    VkDescriptorSet set;
    int dirty;      /* the box drawn into since it last went back to g_vram */
    int dirtyX0, dirtyY0, dirtyX1, dirtyY1;
} Surface;

typedef struct Buffer {
    VkBuffer buffer;
    VkDeviceMemory memory;
    uint8_t *mapped;
    uint32_t size;
} Buffer;

static struct {
    SDL_Window *window;
    int scale;
    VkInstance instance;
    VkSurfaceKHR windowSurface;
    VkPhysicalDevice physical;
    VkDevice device;
    VkQueue queue;
    uint32_t queueFamily;
    VkCommandPool pool;
    VkCommandBuffer cmd;
    VkFence fence;
    VkSemaphore acquired, rendered;
    VkSwapchainKHR swapchain;
    VkFormat swapFormat;
    VkExtent2D swapExtent;
    uint32_t swapCount;
    VkImage swapImages[MAX_SWAPCHAIN_IMAGES];
    VkImageView swapViews[MAX_SWAPCHAIN_IMAGES];
    VkPresentModeKHR presentMode;

    Image vram;              /* g_vram at 1x, R16_UINT */
    Surface surfaces[SURFACE_COUNT];
    Image display24;         /* a 24-bit display, converted on the CPU */
    VkDescriptorSet display24Set;
    Image native;            /* the shown picture at the console's resolution (downsample.frag) */
    VkDescriptorSet nativeSet;
    int downsample, crt, curvature; /* the display settings (VkRendererSetDisplay) */
    Image shown;             /* the picture as shown, for a test's capture (VkRendererCaptureShown) */
    Image overlay;           /* the options overlay (overlay.h), laid over the window */
    VkDescriptorSet overlaySet;
    const uint32_t *overlayPixels; /* NULL: none shown */
    int overlayDirty;
    int vsync;
    int margin;
    Buffer vertices, uploads, readback;
    uint32_t vertexUsed, uploadUsed;

    VkSampler nearest, linear;
    VkDescriptorSetLayout setLayout;
    VkPipelineLayout layout;
    VkPipeline pipelines[PIPE_COUNT];

    int recording;
    int renderingSurface;    /* -1 when not inside a rendering pass */
    int boundPipe, boundPass;
    VkRect2D boundScissor;
    uint32_t batchFirst, batchCount;
    uint32_t *capture;
} V;

static void Check(VkResult result, const char *what)
{
    if (result != VK_SUCCESS) {
        HostFatal("Vulkan: %s failed (%d)", what, (int)result);
    }
}

static uint32_t FindMemory(uint32_t typeBits, VkMemoryPropertyFlags flags)
{
    VkPhysicalDeviceMemoryProperties props;
    uint32_t i;
    vkGetPhysicalDeviceMemoryProperties(V.physical, &props);
    for (i = 0; i < props.memoryTypeCount; i++) {
        if ((typeBits & (1u << i)) && (props.memoryTypes[i].propertyFlags & flags) == flags) {
            return i;
        }
    }
    HostFatal("Vulkan: no suitable memory type");
    return 0;
}

static void CreateImage(Image *img, int width, int height, VkFormat format, VkImageUsageFlags usage)
{
    VkImageCreateInfo info = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    VkMemoryRequirements req;
    VkMemoryAllocateInfo alloc = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    VkImageViewCreateInfo view = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};

    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = format;
    info.extent.width = (uint32_t)width;
    info.extent.height = (uint32_t)height;
    info.extent.depth = 1;
    info.mipLevels = 1;
    info.arrayLayers = 1;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = usage;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    Check(vkCreateImage(V.device, &info, NULL, &img->image), "vkCreateImage");
    vkGetImageMemoryRequirements(V.device, img->image, &req);
    alloc.allocationSize = req.size;
    alloc.memoryTypeIndex = FindMemory(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    Check(vkAllocateMemory(V.device, &alloc, NULL, &img->memory), "vkAllocateMemory");
    vkBindImageMemory(V.device, img->image, img->memory, 0);
    view.image = img->image;
    view.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view.format = format;
    view.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    view.subresourceRange.levelCount = 1;
    view.subresourceRange.layerCount = 1;
    Check(vkCreateImageView(V.device, &view, NULL, &img->view), "vkCreateImageView");
    img->width = width;
    img->height = height;
}

static void CreateBuffer(Buffer *buf, uint32_t size, VkBufferUsageFlags usage)
{
    VkBufferCreateInfo info = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    VkMemoryRequirements req;
    VkMemoryAllocateInfo alloc = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    void *mapped;
    info.size = size;
    info.usage = usage;
    Check(vkCreateBuffer(V.device, &info, NULL, &buf->buffer), "vkCreateBuffer");
    vkGetBufferMemoryRequirements(V.device, buf->buffer, &req);
    alloc.allocationSize = req.size;
    alloc.memoryTypeIndex = FindMemory(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                               VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    Check(vkAllocateMemory(V.device, &alloc, NULL, &buf->memory), "vkAllocateMemory");
    vkBindBufferMemory(V.device, buf->buffer, buf->memory, 0);
    Check(vkMapMemory(V.device, buf->memory, 0, VK_WHOLE_SIZE, 0, &mapped), "vkMapMemory");
    buf->mapped = mapped;
    buf->size = size;
}

/* Every write before it is visible to every access after it. */
static void Barrier(void)
{
    VkMemoryBarrier2 barrier = {VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    VkDependencyInfo dep = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    barrier.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    barrier.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
    barrier.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    barrier.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    dep.memoryBarrierCount = 1;
    dep.pMemoryBarriers = &barrier;
    vkCmdPipelineBarrier2(V.cmd, &dep);
}

static void ImageLayout(VkImage image, VkImageLayout from, VkImageLayout to)
{
    VkImageMemoryBarrier2 barrier = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    VkDependencyInfo dep = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    barrier.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    barrier.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
    barrier.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    barrier.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    barrier.oldLayout = from;
    barrier.newLayout = to;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.layerCount = 1;
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers = &barrier;
    vkCmdPipelineBarrier2(V.cmd, &dep);
}

static void BeginCommands(void)
{
    VkCommandBufferBeginInfo begin = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    if (V.recording) {
        return;
    }
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    Check(vkBeginCommandBuffer(V.cmd, &begin), "vkBeginCommandBuffer");
    V.recording = 1;
    V.renderingSurface = -1;
    V.boundPipe = -1;
}

static void FlushBatch(void)
{
    if (V.batchCount) {
        vkCmdDraw(V.cmd, V.batchCount, 1, V.batchFirst, 0);
        V.batchFirst += V.batchCount;
        V.batchCount = 0;
    }
}

static void EndRendering(void)
{
    if (V.renderingSurface >= 0) {
        FlushBatch();
        vkCmdEndRendering(V.cmd);
        Barrier();
        V.renderingSurface = -1;
        V.boundPipe = -1;
    }
}

/* Submits what is recorded and waits for it; the rings start over. */
static void SubmitAndWait(VkSemaphore wait, VkSemaphore signal)
{
    VkSubmitInfo submit = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
    VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    if (!V.recording) {
        return;
    }
    EndRendering();
    Check(vkEndCommandBuffer(V.cmd), "vkEndCommandBuffer");
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &V.cmd;
    if (wait) {
        submit.waitSemaphoreCount = 1;
        submit.pWaitSemaphores = &wait;
        submit.pWaitDstStageMask = &waitStage;
    }
    if (signal) {
        submit.signalSemaphoreCount = 1;
        submit.pSignalSemaphores = &signal;
    }
    Check(vkQueueSubmit(V.queue, 1, &submit, V.fence), "vkQueueSubmit");
    Check(vkWaitForFences(V.device, 1, &V.fence, VK_TRUE, UINT64_MAX), "vkWaitForFences");
    vkResetFences(V.device, 1, &V.fence);
    V.recording = 0;
    V.vertexUsed = 0;
    V.uploadUsed = 0;
    V.batchFirst = 0;
    V.batchCount = 0;
}

static void BeginRendering(int index)
{
    Surface *s = &V.surfaces[index];
    VkRenderingAttachmentInfo color = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    VkRenderingInfo info = {VK_STRUCTURE_TYPE_RENDERING_INFO};
    VkViewport viewport;
    VkDeviceSize offset = 0;

    if (V.renderingSurface == index) {
        return;
    }
    EndRendering();
    color.imageView = s->image.view;
    color.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    info.renderArea.extent.width = (uint32_t)s->image.width;
    info.renderArea.extent.height = (uint32_t)s->image.height;
    info.layerCount = 1;
    info.colorAttachmentCount = 1;
    info.pColorAttachments = &color;
    vkCmdBeginRendering(V.cmd, &info);
    viewport.x = viewport.y = 0;
    viewport.width = (float)s->image.width;
    viewport.height = (float)s->image.height;
    viewport.minDepth = 0;
    viewport.maxDepth = 1;
    vkCmdSetViewport(V.cmd, 0, 1, &viewport);
    vkCmdBindVertexBuffers(V.cmd, 0, 1, &V.vertices.buffer, &offset);
    vkCmdBindDescriptorSets(V.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, V.layout, 0, 1, &s->set, 0, NULL);
    V.renderingSurface = index;
    V.boundPipe = -1;
}

/* A surface's left edge, margin included, and its whole width, in console pixels. */
static int SurfaceLeft(const Surface *s)
{
    return s->x - s->margin;
}

static int SurfaceWidth(const Surface *s)
{
    return s->w + 2 * s->margin;
}

static int Intersects(int ax0, int ay0, int ax1, int ay1, int bx0, int by0, int bx1, int by1)
{
    return ax0 < bx1 && bx0 < ax1 && ay0 < by1 && by0 < ay1;
}

/* Brings the box a surface has drawn into back to g_vram, at 1x, and to the 1x image. */
static void UploadVram(int x, int y, int w, int h);

static void SyncSurface(int index)
{
    Surface *s = &V.surfaces[index];
    int x0 = s->dirtyX0 > s->x ? s->dirtyX0 : s->x, y0 = s->dirtyY0;
    int x1 = s->dirtyX1 < s->x + s->w ? s->dirtyX1 : s->x + s->w, y1 = s->dirtyY1;
    int scale = V.scale, w = x1 - x0, h = y1 - y0, i, j;
    VkBufferImageCopy copy;
    const uint32_t *pixels;

    if (!s->dirty || w <= 0 || h <= 0) {
        s->dirty = 0;
        return;
    }
    BeginCommands();
    EndRendering();
    memset(&copy, 0, sizeof(copy));
    copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    copy.imageSubresource.layerCount = 1;
    copy.imageOffset.x = (x0 - SurfaceLeft(s)) * scale;
    copy.imageOffset.y = (y0 - s->y) * scale;
    copy.imageExtent.width = (uint32_t)(w * scale);
    copy.imageExtent.height = (uint32_t)(h * scale);
    copy.imageExtent.depth = 1;
    vkCmdCopyImageToBuffer(V.cmd, s->image.image, VK_IMAGE_LAYOUT_GENERAL, V.readback.buffer, 1, &copy);
    Barrier();
    SubmitAndWait(VK_NULL_HANDLE, VK_NULL_HANDLE);
    pixels = (const uint32_t *)V.readback.mapped;
    for (j = 0; j < h; j++) {
        for (i = 0; i < w; i++) {
            uint32_t p = pixels[(j * scale + scale / 2) * (w * scale) + i * scale + scale / 2];
            uint16_t c = (uint16_t)(((p >> 3) & 31) | ((p >> 11) & 31) << 5 | ((p >> 19) & 31) << 10);
            g_vram[(y0 + j) & 511][(x0 + i) & 1023] = c;
        }
    }
    s->dirty = 0;
    UploadVram(x0, y0, w, h);
}

static void SyncRect(int x, int y, int w, int h)
{
    int i;
    for (i = 0; i < SURFACE_COUNT; i++) {
        Surface *s = &V.surfaces[i];
        if (s->dirty && Intersects(x, y, x + w, y + h, s->dirtyX0, s->dirtyY0, s->dirtyX1, s->dirtyY1)) {
            SyncSurface(i);
        }
    }
}

/* Copies a rectangle of g_vram into the 1x image through the upload ring. */
static void UploadVram(int x, int y, int w, int h)
{
    VkBufferImageCopy copy;
    uint32_t bytes, j;
    uint16_t *dst;

    if (x < 0) w += x, x = 0;
    if (y < 0) h += y, y = 0;
    if (x + w > VRAM_WIDTH) w = VRAM_WIDTH - x;
    if (y + h > VRAM_HEIGHT) h = VRAM_HEIGHT - y;
    if (w <= 0 || h <= 0) {
        return;
    }
    bytes = (uint32_t)(w * h * 2 + 3) & ~3u;
    if (V.uploadUsed + bytes > V.uploads.size) {
        SubmitAndWait(VK_NULL_HANDLE, VK_NULL_HANDLE);
    }
    BeginCommands();
    EndRendering();
    dst = (uint16_t *)(V.uploads.mapped + V.uploadUsed);
    for (j = 0; j < (uint32_t)h; j++) {
        memcpy(dst + j * w, &g_vram[y + j][x], (size_t)w * 2);
    }
    memset(&copy, 0, sizeof(copy));
    copy.bufferOffset = V.uploadUsed;
    copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    copy.imageSubresource.layerCount = 1;
    copy.imageOffset.x = x;
    copy.imageOffset.y = y;
    copy.imageExtent.width = (uint32_t)w;
    copy.imageExtent.height = (uint32_t)h;
    copy.imageExtent.depth = 1;
    vkCmdCopyBufferToImage(V.cmd, V.uploads.buffer, V.vram.image, VK_IMAGE_LAYOUT_GENERAL, 1, &copy);
    Barrier();
    V.uploadUsed += bytes;
}

static void PushConstants(const Push *push)
{
    vkCmdPushConstants(V.cmd, V.layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                       sizeof(*push), push);
}

/* Paints an uploaded rectangle into every surface it overlaps. */
static void PaintSurfaces(int x, int y, int w, int h)
{
    int i;
    for (i = 0; i < SURFACE_COUNT; i++) {
        Surface *s = &V.surfaces[i];
        int x0 = x > s->x ? x : s->x, y0 = y > s->y ? y : s->y;
        int x1 = x + w < s->x + s->w ? x + w : s->x + s->w, y1 = y + h < s->y + s->h ? y + h : s->y + s->h;
        VkRect2D scissor;
        Push push;
        if (x0 >= x1 || y0 >= y1) {
            continue;
        }
        BeginRendering(i);
        FlushBatch();
        vkCmdBindPipeline(V.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, V.pipelines[PIPE_PAINT]);
        V.boundPipe = -1;
        scissor.offset.x = (x0 - SurfaceLeft(s)) * V.scale;
        scissor.offset.y = (y0 - s->y) * V.scale;
        scissor.extent.width = (uint32_t)((x1 - x0) * V.scale);
        scissor.extent.height = (uint32_t)((y1 - y0) * V.scale);
        vkCmdSetScissor(V.cmd, 0, 1, &scissor);
        push.rect[0] = -1, push.rect[1] = -1, push.rect[2] = 1, push.rect[3] = 1;
        push.extra[0] = (float)SurfaceLeft(s), push.extra[1] = (float)s->y;
        push.extra[2] = (float)V.scale, push.extra[3] = 0;
        PushConstants(&push);
        vkCmdDraw(V.cmd, 4, 1, 0, 0);
        /* What was drawn there is replaced: if it covered the whole drawn box, nothing is left to bring back. */
        if (s->dirty && x0 <= s->dirtyX0 && y0 <= s->dirtyY0 && x1 >= s->dirtyX1 && y1 >= s->dirtyY1) {
            s->dirty = 0;
        }
    }
}

static void HwVramWritten(int x, int y, int w, int h)
{
    UploadVram(x, y, w, h);
    PaintSurfaces(x, y, w, h);
}

static void HwFill(int x, int y, int w, int h, uint32_t rgb)
{
    int i;
    UploadVram(x, y, w, h);
    for (i = 0; i < SURFACE_COUNT; i++) {
        Surface *s = &V.surfaces[i];
        int x0 = x > s->x ? x : s->x, y0 = y > s->y ? y : s->y;
        int x1 = x + w < s->x + s->w ? x + w : s->x + s->w, y1 = y + h < s->y + s->h ? y + h : s->y + s->h;
        VkClearAttachment clear;
        VkClearRect rect;
        if (x0 >= x1 || y0 >= y1) {
            continue;
        }
        if (s->margin && x0 == s->x && x1 == s->x + s->w) {
            /* The whole width: the margins too. */
            x0 = SurfaceLeft(s);
            x1 = s->x + s->w + s->margin;
        }
        BeginRendering(i);
        FlushBatch();
        memset(&clear, 0, sizeof(clear));
        clear.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        clear.clearValue.color.float32[0] = ((rgb >> 3) & 31) / 31.0f;
        clear.clearValue.color.float32[1] = ((rgb >> 11) & 31) / 31.0f;
        clear.clearValue.color.float32[2] = ((rgb >> 19) & 31) / 31.0f;
        clear.clearValue.color.float32[3] = 1.0f;
        rect.rect.offset.x = (x0 - SurfaceLeft(s)) * V.scale;
        rect.rect.offset.y = (y0 - s->y) * V.scale;
        rect.rect.extent.width = (uint32_t)((x1 - x0) * V.scale);
        rect.rect.extent.height = (uint32_t)((y1 - y0) * V.scale);
        rect.baseArrayLayer = 0;
        rect.layerCount = 1;
        vkCmdClearAttachments(V.cmd, 1, &clear, 1, &rect);
        if (s->dirty && x0 <= s->dirtyX0 && y0 <= s->dirtyY0 && x1 >= s->dirtyX1 && y1 >= s->dirtyY1) {
            s->dirty = 0;
        }
    }
}

static void HwVramRead(int x, int y, int w, int h)
{
    SyncRect(x, y, w, h);
}

static int SurfaceFor(const GpuDrawState *st)
{
    int i;
    for (i = SURFACE_FB0; i <= SURFACE_FB1; i++) {
        Surface *s = &V.surfaces[i];
        if (st->clipX1 >= s->x && st->clipY1 >= s->y && st->clipX2 < s->x + s->w && st->clipY2 < s->y + s->h) {
            return i;
        }
    }
    return SURFACE_VRAM;
}

static void Draw(int surfaceIndex, int pipe, int pass, const VkRect2D *scissor, const Vertex *v)
{
    Surface *s = &V.surfaces[surfaceIndex];
    if (V.vertexUsed + 3 * sizeof(Vertex) > V.vertices.size) {
        SubmitAndWait(VK_NULL_HANDLE, VK_NULL_HANDLE);
    }
    BeginCommands();
    BeginRendering(surfaceIndex);
    if (V.boundPipe != pipe || V.boundPass != pass || memcmp(&V.boundScissor, scissor, sizeof(*scissor)) != 0) {
        Push push;
        FlushBatch();
        if (V.boundPipe != pipe) {
            static const float constants[PIPE_COUNT] = {0, 0.5f, 0, 0, 0.25f};
            float blend[4];
            vkCmdBindPipeline(V.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, V.pipelines[pipe]);
            blend[0] = blend[1] = blend[2] = blend[3] = constants[pipe];
            vkCmdSetBlendConstants(V.cmd, blend);
        }
        vkCmdSetScissor(V.cmd, 0, 1, scissor);
        push.rect[0] = (float)SurfaceLeft(s), push.rect[1] = (float)s->y;
        push.rect[2] = (float)SurfaceWidth(s), push.rect[3] = (float)s->h;
        push.extra[0] = (float)pass, push.extra[1] = push.extra[2] = push.extra[3] = 0;
        PushConstants(&push);
        V.boundPipe = pipe;
        V.boundPass = pass;
        V.boundScissor = *scissor;
    }
    memcpy(V.vertices.mapped + V.vertexUsed, v, 3 * sizeof(Vertex));
    if (V.batchCount == 0) {
        V.batchFirst = V.vertexUsed / sizeof(Vertex);
    }
    V.vertexUsed += 3 * sizeof(Vertex);
    V.batchCount += 3;
}

static void HwTriangle(const GpuVertex in[3], const GpuDrawState *st)
{
    int index = SurfaceFor(st);
    Surface *s = &V.surfaces[index];
    Vertex v[3];
    VkRect2D scissor;
    uint32_t texture[4];
    int i, x0, y0, x1, y1;

    if (st->textured) {
        /* A texture or colour table that lies where something was drawn is brought back first. */
        static const int pageWidths[3] = {64, 128, 256};
        int pageW = pageWidths[st->texDepth > 2 ? 2 : st->texDepth];
        SyncRect(st->texX, st->texY, pageW, 256);
        if (st->texDepth < 2) {
            SyncRect(st->clutX, st->clutY, st->texDepth == 0 ? 16 : 256, 1);
        }
    }
    x0 = st->clipX1 > s->x ? st->clipX1 : s->x;
    y0 = st->clipY1 > s->y ? st->clipY1 : s->y;
    x1 = st->clipX2 + 1 < s->x + s->w ? st->clipX2 + 1 : s->x + s->w;
    y1 = st->clipY2 + 1 < s->y + s->h ? st->clipY2 + 1 : s->y + s->h;
    if (x0 >= x1 || y0 >= y1) {
        return;
    }
    if (s->margin && x0 == s->x && x1 == s->x + s->w) {
        x0 = SurfaceLeft(s);
        x1 = s->x + s->w + s->margin;
    }
    scissor.offset.x = (x0 - SurfaceLeft(s)) * V.scale;
    scissor.offset.y = (y0 - s->y) * V.scale;
    scissor.extent.width = (uint32_t)((x1 - x0) * V.scale);
    scissor.extent.height = (uint32_t)((y1 - y0) * V.scale);

    texture[0] = (uint32_t)(uint16_t)st->texX | (uint32_t)(uint16_t)st->texY << 16;
    texture[1] = (uint32_t)(uint16_t)st->clutX | (uint32_t)(uint16_t)st->clutY << 16;
    texture[2] = st->texDepth | (uint32_t)(st->textured | st->raw << 1 | st->semi << 2) << 8;
    texture[3] = st->twMaskX | (uint32_t)st->twMaskY << 8 | (uint32_t)st->twOffX << 16 | (uint32_t)st->twOffY << 24;
    {
        /* A full-screen fill (untextured, corners exactly on the buffer's edges) covers the margins too. */
        int stretch = 0;
        if (s->margin && !st->textured) {
            float minX = in[0].x, maxX = in[0].x;
            for (i = 1; i < 3; i++) {
                minX = in[i].x < minX ? in[i].x : minX;
                maxX = in[i].x > maxX ? in[i].x : maxX;
            }
            stretch = minX == (float)s->x && (maxX == (float)(s->x + s->w) || maxX == (float)(s->x + s->w - 1));
        }
        for (i = 0; i < 3; i++) {
            v[i].x = in[i].x;
            if (stretch && in[i].x == (float)s->x) {
                v[i].x = (float)SurfaceLeft(s);
            } else if (stretch && in[i].x >= (float)(s->x + s->w - 1)) {
                v[i].x = (float)(s->x + s->w + s->margin);
            }
        }
    }
    for (i = 0; i < 3; i++) {
        v[i].y = in[i].y;
        v[i].r = in[i].r;
        v[i].g = in[i].g;
        v[i].b = in[i].b;
        v[i].a = 255;
        v[i].u = in[i].u;
        v[i].v = in[i].v;
        memcpy(v[i].texture, texture, sizeof(texture));
    }
    if (!st->semi) {
        Draw(index, PIPE_OPAQUE, 2, &scissor, v);
    } else {
        if (st->textured) {
            Draw(index, PIPE_OPAQUE, 0, &scissor, v); /* its opaque texels */
        }
        Draw(index, PIPE_SEMI0 + st->semiMode, 1, &scissor, v);
    }
    /* The box drawn into, for bringing it back to g_vram when the game reads it: video memory's part of
     * the surface only, as the margins are no part of it. */
    if (x0 < s->x) x0 = s->x;
    if (x1 > s->x + s->w) x1 = s->x + s->w;
    if (x0 >= x1) {
        return;
    }
    if (!s->dirty) {
        s->dirty = 1;
        s->dirtyX0 = x0, s->dirtyY0 = y0, s->dirtyX1 = x1, s->dirtyY1 = y1;
    } else {
        if (x0 < s->dirtyX0) s->dirtyX0 = x0;
        if (y0 < s->dirtyY0) s->dirtyY0 = y0;
        if (x1 > s->dirtyX1) s->dirtyX1 = x1;
        if (y1 > s->dirtyY1) s->dirtyY1 = y1;
    }
}

static const GpuRenderer sRenderer = {HwTriangle, HwFill, HwVramWritten, HwVramRead};

/* --- setup ------------------------------------------------------------------------------------------- */

static VkShaderModule Shader(const uint32_t *code, size_t size)
{
    VkShaderModuleCreateInfo info = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    VkShaderModule module;
    info.codeSize = size;
    info.pCode = code;
    Check(vkCreateShaderModule(V.device, &info, NULL, &module), "vkCreateShaderModule");
    return module;
}

static VkPipeline CreatePipeline(VkShaderModule vert, VkShaderModule frag, int withVertices, VkFormat format,
                                 int blendMode)
{
    VkPipelineShaderStageCreateInfo stages[2] = {{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO},
                                                 {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}};
    VkVertexInputBindingDescription binding = {0, sizeof(Vertex), VK_VERTEX_INPUT_RATE_VERTEX};
    VkVertexInputAttributeDescription attrs[4] = {
        {0, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(Vertex, x)},
        {1, 0, VK_FORMAT_R8G8B8A8_UNORM, offsetof(Vertex, r)},
        {2, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(Vertex, u)},
        {3, 0, VK_FORMAT_R32G32B32A32_UINT, offsetof(Vertex, texture)},
    };
    VkPipelineVertexInputStateCreateInfo input = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo assembly = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    VkPipelineViewportStateCreateInfo viewport = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    VkPipelineRasterizationStateCreateInfo raster = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    VkPipelineMultisampleStateCreateInfo multisample = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    VkPipelineColorBlendAttachmentState attachment;
    VkPipelineColorBlendStateCreateInfo blend = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    VkDynamicState dynamics[3] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR,
                                  VK_DYNAMIC_STATE_BLEND_CONSTANTS};
    VkPipelineDynamicStateCreateInfo dynamic = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    VkPipelineRenderingCreateInfo rendering = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    VkGraphicsPipelineCreateInfo info = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    VkPipeline pipeline;

    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vert;
    stages[0].pName = "main";
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = frag;
    stages[1].pName = "main";
    if (withVertices) {
        input.vertexBindingDescriptionCount = 1;
        input.pVertexBindingDescriptions = &binding;
        input.vertexAttributeDescriptionCount = 4;
        input.pVertexAttributeDescriptions = attrs;
    }
    assembly.topology = withVertices ? VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
    viewport.viewportCount = 1;
    viewport.scissorCount = 1;
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = VK_CULL_MODE_NONE;
    raster.lineWidth = 1.0f;
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    memset(&attachment, 0, sizeof(attachment));
    attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT |
                                VK_COLOR_COMPONENT_A_BIT;
    if (blendMode == 4) {
        /* The overlay: ordinary transparency. */
        attachment.blendEnable = VK_TRUE;
        attachment.colorBlendOp = attachment.alphaBlendOp = VK_BLEND_OP_ADD;
        attachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        attachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        attachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        attachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    } else if (blendMode >= 0) {
        /* The console's four semi-transparency modes: B/2+F/2, B+F, B-F, B+F/4. */
        attachment.blendEnable = VK_TRUE;
        attachment.colorBlendOp = blendMode == 2 ? VK_BLEND_OP_REVERSE_SUBTRACT : VK_BLEND_OP_ADD;
        attachment.srcColorBlendFactor =
            (blendMode == 0 || blendMode == 3) ? VK_BLEND_FACTOR_CONSTANT_COLOR : VK_BLEND_FACTOR_ONE;
        attachment.dstColorBlendFactor = blendMode == 0 ? VK_BLEND_FACTOR_CONSTANT_COLOR : VK_BLEND_FACTOR_ONE;
        attachment.alphaBlendOp = VK_BLEND_OP_ADD;
        attachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        attachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    }
    blend.attachmentCount = 1;
    blend.pAttachments = &attachment;
    dynamic.dynamicStateCount = 3;
    dynamic.pDynamicStates = dynamics;
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachmentFormats = &format;
    info.pNext = &rendering;
    info.stageCount = 2;
    info.pStages = stages;
    info.pVertexInputState = &input;
    info.pInputAssemblyState = &assembly;
    info.pViewportState = &viewport;
    info.pRasterizationState = &raster;
    info.pMultisampleState = &multisample;
    info.pColorBlendState = &blend;
    info.pDynamicState = &dynamic;
    info.layout = V.layout;
    Check(vkCreateGraphicsPipelines(V.device, VK_NULL_HANDLE, 1, &info, NULL, &pipeline), "vkCreateGraphicsPipelines");
    return pipeline;
}

static void CreateSwapchain(void)
{
    VkSurfaceCapabilitiesKHR caps;
    VkSurfaceFormatKHR formats[32];
    VkPresentModeKHR modes[8];
    uint32_t formatCount = 32, modeCount = 8, i;
    VkSwapchainCreateInfoKHR info = {VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    VkSwapchainKHR old = V.swapchain;

    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(V.physical, V.windowSurface, &caps);
    vkGetPhysicalDeviceSurfaceFormatsKHR(V.physical, V.windowSurface, &formatCount, formats);
    vkGetPhysicalDeviceSurfacePresentModesKHR(V.physical, V.windowSurface, &modeCount, modes);
    V.swapFormat = formats[0].format;
    for (i = 0; i < formatCount; i++) {
        if (formats[i].format == VK_FORMAT_B8G8R8A8_UNORM || formats[i].format == VK_FORMAT_R8G8B8A8_UNORM) {
            V.swapFormat = formats[i].format;
            break;
        }
    }
    /* The host paces frames itself. Without vsync a picture goes to the display the moment it is done
     * (immediate: the least latency, at the cost of tearing; else mailbox, which holds it to the refresh);
     * with it, each picture waits for the display's refresh. */
    V.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    if (!V.vsync) {
        for (i = 0; i < modeCount; i++) {
            if (modes[i] == VK_PRESENT_MODE_MAILBOX_KHR && V.presentMode == VK_PRESENT_MODE_FIFO_KHR) {
                V.presentMode = modes[i];
            }
        }
        for (i = 0; i < modeCount; i++) {
            if (modes[i] == VK_PRESENT_MODE_IMMEDIATE_KHR) {
                V.presentMode = modes[i];
            }
        }
    }
    if (caps.currentExtent.width != UINT32_MAX) {
        V.swapExtent = caps.currentExtent;
    } else {
        int w, h;
        SDL_GetWindowSizeInPixels(V.window, &w, &h);
        V.swapExtent.width = (uint32_t)w;
        V.swapExtent.height = (uint32_t)h;
    }
    if (V.swapExtent.width == 0 || V.swapExtent.height == 0) {
        return; /* minimized */
    }
    info.surface = V.windowSurface;
    /* As few images as the display allows (two, double buffering): every frame is finished before the next
     * begins (SubmitAndWait), so a third would only let one more picture wait in the vsync queue, a refresh
     * of latency. */
    info.minImageCount = caps.minImageCount > 2 ? caps.minImageCount : 2;
    if (caps.maxImageCount && info.minImageCount > caps.maxImageCount) {
        info.minImageCount = caps.maxImageCount;
    }
    info.imageFormat = V.swapFormat;
    info.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    info.imageExtent = V.swapExtent;
    info.imageArrayLayers = 1;
    info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.preTransform = caps.currentTransform;
    info.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    info.presentMode = V.presentMode;
    info.clipped = VK_TRUE;
    info.oldSwapchain = old;
    Check(vkCreateSwapchainKHR(V.device, &info, NULL, &V.swapchain), "vkCreateSwapchainKHR");
    if (old) {
        for (i = 0; i < V.swapCount; i++) {
            vkDestroyImageView(V.device, V.swapViews[i], NULL);
        }
        vkDestroySwapchainKHR(V.device, old, NULL);
    }
    V.swapCount = MAX_SWAPCHAIN_IMAGES;
    vkGetSwapchainImagesKHR(V.device, V.swapchain, &V.swapCount, V.swapImages);
    for (i = 0; i < V.swapCount; i++) {
        VkImageViewCreateInfo view = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        view.image = V.swapImages[i];
        view.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view.format = V.swapFormat;
        view.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        view.subresourceRange.levelCount = 1;
        view.subresourceRange.layerCount = 1;
        Check(vkCreateImageView(V.device, &view, NULL, &V.swapViews[i]), "vkCreateImageView");
    }
}

static void WriteDescriptorSampled(VkDescriptorSet set, VkImageView source, VkSampler sampler);

static void WriteDescriptor(VkDescriptorSet set, VkImageView source)
{
    WriteDescriptorSampled(set, source, V.linear);
}

static void WriteDescriptorSampled(VkDescriptorSet set, VkImageView source, VkSampler sampler)
{
    VkDescriptorImageInfo images[2];
    VkWriteDescriptorSet writes[2] = {{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}, {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}};
    images[0].sampler = V.nearest;
    images[0].imageView = V.vram.view;
    images[0].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    images[1].sampler = sampler;
    images[1].imageView = source;
    images[1].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    writes[0].dstSet = set;
    writes[0].dstBinding = 0;
    writes[0].descriptorCount = 1;
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[0].pImageInfo = &images[0];
    writes[1] = writes[0];
    writes[1].dstBinding = 1;
    writes[1].pImageInfo = &images[1];
    vkUpdateDescriptorSets(V.device, 2, writes, 0, NULL);
}

/* The extensions a device older than Vulkan 1.3 needs for what 1.3 has built in (dynamic rendering and
 * synchronization2), with the ones those depend on. A device with all of them draws as a 1.3 one does: the entry
 * points are the same, found under their KHR names. DOJO_VK_KHR=1 takes this road on a 1.3 device too, to try it. */
static const char *const kKhrNeeded[] = {VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME,
                                         VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME};
static const char *const kKhrDepends[] = {VK_KHR_DEPTH_STENCIL_RESOLVE_EXTENSION_NAME,
                                          VK_KHR_CREATE_RENDERPASS_2_EXTENSION_NAME,
                                          VK_KHR_MULTIVIEW_EXTENSION_NAME, VK_KHR_MAINTENANCE_2_EXTENSION_NAME};

static int DeviceHas(VkPhysicalDevice dev, const char *name)
{
    VkExtensionProperties list[512];
    uint32_t count = 512, i;
    if (vkEnumerateDeviceExtensionProperties(dev, NULL, &count, list) < 0) {
        return 0;
    }
    for (i = 0; i < count; i++) {
        if (strcmp(list[i].extensionName, name) == 0) {
            return 1;
        }
    }
    return 0;
}

/* Whether the device can draw: Vulkan 1.3, or 1.1 and up with the extensions that give the same (set in useKhr). */
static int DeviceUsable(VkPhysicalDevice dev, const VkPhysicalDeviceProperties *props, int *useKhr)
{
    uint32_t i;
    *useKhr = SDL_getenv("DOJO_VK_KHR") != NULL || props->apiVersion < VK_API_VERSION_1_3;
    if (props->apiVersion < VK_API_VERSION_1_1) {
        return 0;
    }
    if (!*useKhr) {
        return 1;
    }
    for (i = 0; i < sizeof(kKhrNeeded) / sizeof(kKhrNeeded[0]); i++) {
        if (!DeviceHas(dev, kKhrNeeded[i])) {
            return 0;
        }
    }
    return 1;
}

int VkRendererInit(SDL_Window *window, int scale, int margin)
{
    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO};
    VkInstanceCreateInfo instanceInfo = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    Uint32 extensionCount = 0;
    char const *const *extensions;
    VkPhysicalDevice devices[8];
    uint32_t deviceCount = 8, i;
    float priority = 1.0f;
    VkDeviceQueueCreateInfo queueInfo = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    VkPhysicalDeviceVulkan13Features features13 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceDynamicRenderingFeaturesKHR dynamicRendering = {
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES_KHR};
    VkPhysicalDeviceSynchronization2FeaturesKHR synchronization2 = {
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES_KHR};
    const char *deviceExtensions[1 + 2 + 4];
    uint32_t deviceExtensionCount = 0;
    int useKhr = 0;
    VkDeviceCreateInfo deviceInfo = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    const char *swapchainExtension = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
    VkCommandPoolCreateInfo poolInfo = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    VkCommandBufferAllocateInfo cmdInfo = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    VkFenceCreateInfo fenceInfo = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkSemaphoreCreateInfo semInfo = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    VkSamplerCreateInfo samplerInfo = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    VkDescriptorSetLayoutBinding bindings[2];
    VkDescriptorSetLayoutCreateInfo setLayoutInfo = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    VkPushConstantRange pushRange;
    VkPipelineLayoutCreateInfo layoutInfo = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    VkDescriptorPoolSize poolSize = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 16};
    VkDescriptorPoolCreateInfo descPoolInfo = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    VkDescriptorPool descPool;
    VkDescriptorSetAllocateInfo setInfo = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    VkShaderModule drawVert, drawFrag, quadVert, paintFrag, downsampleFrag, presentFrag, overlayFrag;
    static const int fbRects[SURFACE_COUNT][4] = {{0, 0, 1024, 512}, {0, 0, 640, 240}, {0, 240, 640, 240}};
    uint32_t readbackBytes;
    VkClearColorValue black;

    memset(&V, 0, sizeof(V));
    V.window = window;
    V.scale = scale < 1 ? 1 : scale;
    V.margin = margin;
    vkGetInstanceProcAddr = (PFN_vkGetInstanceProcAddr)SDL_Vulkan_GetVkGetInstanceProcAddr();
    if (!vkGetInstanceProcAddr) {
        return 0;
    }
    vkCreateInstance = (PFN_vkCreateInstance)vkGetInstanceProcAddr(NULL, "vkCreateInstance");
    extensions = SDL_Vulkan_GetInstanceExtensions(&extensionCount);
    app.pApplicationName = "Dojo";
    app.apiVersion = VK_API_VERSION_1_3;
    instanceInfo.pApplicationInfo = &app;
    instanceInfo.enabledExtensionCount = extensionCount;
    instanceInfo.ppEnabledExtensionNames = extensions;
    if (!vkCreateInstance || vkCreateInstance(&instanceInfo, NULL, &V.instance) != VK_SUCCESS) {
        return 0;
    }
#define LOAD_INSTANCE(name) name = (PFN_##name)vkGetInstanceProcAddr(V.instance, #name);
    VK_INSTANCE_FUNCTIONS(LOAD_INSTANCE)
#undef LOAD_INSTANCE
    if (!SDL_Vulkan_CreateSurface(window, V.instance, NULL, &V.windowSurface)) {
        return 0;
    }

    /* The first device with a queue that draws and presents to the window, discrete GPUs first. */
    vkEnumeratePhysicalDevices(V.instance, &deviceCount, devices);
    for (i = 0; i < deviceCount * 2 && !V.physical; i++) {
        VkPhysicalDevice dev = devices[i % deviceCount];
        VkPhysicalDeviceProperties props;
        VkQueueFamilyProperties families[16];
        uint32_t familyCount = 16, f;
        vkGetPhysicalDeviceProperties(dev, &props);
        if ((i < deviceCount) != (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) ||
            !DeviceUsable(dev, &props, &useKhr)) {
            continue;
        }
        vkGetPhysicalDeviceQueueFamilyProperties(dev, &familyCount, families);
        for (f = 0; f < familyCount; f++) {
            VkBool32 present = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(dev, f, V.windowSurface, &present);
            if ((families[f].queueFlags & VK_QUEUE_GRAPHICS_BIT) && present) {
                V.physical = dev;
                V.queueFamily = f;
                fprintf(stderr, "vulkan: %s (%u.%u%s)\n", props.deviceName, VK_VERSION_MAJOR(props.apiVersion),
                        VK_VERSION_MINOR(props.apiVersion), useKhr ? ", through extensions" : "");
                break;
            }
        }
    }
    if (!V.physical) {
        fprintf(stderr, "vulkan: no graphics card here has Vulkan 1.3, or 1.1 with dynamic rendering and "
                        "synchronization2: using the software renderer\n");
        return 0;
    }
    queueInfo.queueFamilyIndex = V.queueFamily;
    queueInfo.queueCount = 1;
    queueInfo.pQueuePriorities = &priority;
    deviceExtensions[deviceExtensionCount++] = swapchainExtension;
    if (useKhr) {
        /* the 1.3 core structures can't be asked of an older device: the same features, one structure each */
        uint32_t k;
        dynamicRendering.dynamicRendering = VK_TRUE;
        synchronization2.synchronization2 = VK_TRUE;
        dynamicRendering.pNext = &synchronization2;
        deviceInfo.pNext = &dynamicRendering;
        for (k = 0; k < sizeof(kKhrNeeded) / sizeof(kKhrNeeded[0]); k++) {
            deviceExtensions[deviceExtensionCount++] = kKhrNeeded[k];
        }
        for (k = 0; k < sizeof(kKhrDepends) / sizeof(kKhrDepends[0]); k++) {
            if (DeviceHas(V.physical, kKhrDepends[k])) {
                deviceExtensions[deviceExtensionCount++] = kKhrDepends[k];
            }
        }
    } else {
        features13.dynamicRendering = VK_TRUE;
        features13.synchronization2 = VK_TRUE;
        deviceInfo.pNext = &features13;
    }
    deviceInfo.queueCreateInfoCount = 1;
    deviceInfo.pQueueCreateInfos = &queueInfo;
    deviceInfo.enabledExtensionCount = deviceExtensionCount;
    deviceInfo.ppEnabledExtensionNames = deviceExtensions;
    if (vkCreateDevice(V.physical, &deviceInfo, NULL, &V.device) != VK_SUCCESS) {
        return 0;
    }
#define LOAD_DEVICE(name) name = (PFN_##name)vkGetDeviceProcAddr(V.device, #name);
    VK_DEVICE_FUNCTIONS(LOAD_DEVICE)
#undef LOAD_DEVICE
    if (useKhr) {
        vkCmdBeginRendering = (PFN_vkCmdBeginRendering)vkGetDeviceProcAddr(V.device, "vkCmdBeginRenderingKHR");
        vkCmdEndRendering = (PFN_vkCmdEndRendering)vkGetDeviceProcAddr(V.device, "vkCmdEndRenderingKHR");
        vkCmdPipelineBarrier2 = (PFN_vkCmdPipelineBarrier2)vkGetDeviceProcAddr(V.device, "vkCmdPipelineBarrier2KHR");
        if (!vkCmdBeginRendering || !vkCmdEndRendering || !vkCmdPipelineBarrier2) {
            return 0;
        }
    }
    vkGetDeviceQueue(V.device, V.queueFamily, 0, &V.queue);

    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolInfo.queueFamilyIndex = V.queueFamily;
    Check(vkCreateCommandPool(V.device, &poolInfo, NULL, &V.pool), "vkCreateCommandPool");
    cmdInfo.commandPool = V.pool;
    cmdInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cmdInfo.commandBufferCount = 1;
    Check(vkAllocateCommandBuffers(V.device, &cmdInfo, &V.cmd), "vkAllocateCommandBuffers");
    Check(vkCreateFence(V.device, &fenceInfo, NULL, &V.fence), "vkCreateFence");
    Check(vkCreateSemaphore(V.device, &semInfo, NULL, &V.acquired), "vkCreateSemaphore");
    Check(vkCreateSemaphore(V.device, &semInfo, NULL, &V.rendered), "vkCreateSemaphore");
    CreateSwapchain();

    /* Video memory at 1x, the surfaces at scale, and the 24-bit display. */
    CreateImage(&V.vram, VRAM_WIDTH, VRAM_HEIGHT, VK_FORMAT_R16_UINT,
                VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    for (i = 0; i < SURFACE_COUNT; i++) {
        Surface *s = &V.surfaces[i];
        s->x = fbRects[i][0], s->y = fbRects[i][1], s->w = fbRects[i][2], s->h = fbRects[i][3];
        s->margin = i == SURFACE_VRAM ? 0 : margin;
        CreateImage(&s->image, SurfaceWidth(s) * V.scale, s->h * V.scale, VK_FORMAT_R8G8B8A8_UNORM,
                    VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                        VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    }
    CreateImage(&V.display24, 640, 480, VK_FORMAT_R8G8B8A8_UNORM,
                VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    CreateImage(&V.overlay, OVERLAY_WIDTH, OVERLAY_HEIGHT, VK_FORMAT_R8G8B8A8_UNORM,
                VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    CreateImage(&V.native, NATIVE_WIDTH, NATIVE_HEIGHT, VK_FORMAT_R8G8B8A8_UNORM,
                VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
    readbackBytes = (uint32_t)((VRAM_WIDTH + 2 * margin) * V.scale) * (uint32_t)(VRAM_HEIGHT * V.scale) * 4;
    CreateBuffer(&V.vertices, VERTEX_RING_BYTES, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
    CreateBuffer(&V.uploads, UPLOAD_RING_BYTES, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    CreateBuffer(&V.readback, readbackBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT);

    samplerInfo.magFilter = samplerInfo.minFilter = VK_FILTER_NEAREST;
    samplerInfo.addressModeU = samplerInfo.addressModeV = samplerInfo.addressModeW =
        VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    Check(vkCreateSampler(V.device, &samplerInfo, NULL, &V.nearest), "vkCreateSampler");
    samplerInfo.magFilter = samplerInfo.minFilter = VK_FILTER_LINEAR;
    Check(vkCreateSampler(V.device, &samplerInfo, NULL, &V.linear), "vkCreateSampler");

    memset(bindings, 0, sizeof(bindings));
    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    bindings[1] = bindings[0];
    bindings[1].binding = 1;
    setLayoutInfo.bindingCount = 2;
    setLayoutInfo.pBindings = bindings;
    Check(vkCreateDescriptorSetLayout(V.device, &setLayoutInfo, NULL, &V.setLayout), "vkCreateDescriptorSetLayout");
    pushRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    pushRange.offset = 0;
    pushRange.size = sizeof(Push);
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts = &V.setLayout;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &pushRange;
    Check(vkCreatePipelineLayout(V.device, &layoutInfo, NULL, &V.layout), "vkCreatePipelineLayout");

    descPoolInfo.maxSets = 8;
    descPoolInfo.poolSizeCount = 1;
    descPoolInfo.pPoolSizes = &poolSize;
    Check(vkCreateDescriptorPool(V.device, &descPoolInfo, NULL, &descPool), "vkCreateDescriptorPool");
    setInfo.descriptorPool = descPool;
    setInfo.descriptorSetCount = 1;
    setInfo.pSetLayouts = &V.setLayout;
    for (i = 0; i < SURFACE_COUNT; i++) {
        Check(vkAllocateDescriptorSets(V.device, &setInfo, &V.surfaces[i].set), "vkAllocateDescriptorSets");
        WriteDescriptor(V.surfaces[i].set, V.surfaces[i].image.view);
    }
    Check(vkAllocateDescriptorSets(V.device, &setInfo, &V.display24Set), "vkAllocateDescriptorSets");
    WriteDescriptor(V.display24Set, V.display24.view);
    Check(vkAllocateDescriptorSets(V.device, &setInfo, &V.nativeSet), "vkAllocateDescriptorSets");
    WriteDescriptor(V.nativeSet, V.native.view);
    Check(vkAllocateDescriptorSets(V.device, &setInfo, &V.overlaySet), "vkAllocateDescriptorSets");
    WriteDescriptorSampled(V.overlaySet, V.overlay.view, V.nearest);

    drawVert = Shader(spv_draw_vert, sizeof(spv_draw_vert));
    drawFrag = Shader(spv_draw_frag, sizeof(spv_draw_frag));
    quadVert = Shader(spv_quad_vert, sizeof(spv_quad_vert));
    paintFrag = Shader(spv_paint_frag, sizeof(spv_paint_frag));
    downsampleFrag = Shader(spv_downsample_frag, sizeof(spv_downsample_frag));
    presentFrag = Shader(spv_present_frag, sizeof(spv_present_frag));
    overlayFrag = Shader(spv_overlay_frag, sizeof(spv_overlay_frag));
    V.pipelines[PIPE_OPAQUE] = CreatePipeline(drawVert, drawFrag, 1, VK_FORMAT_R8G8B8A8_UNORM, -1);
    for (i = 0; i < 4; i++) {
        V.pipelines[PIPE_SEMI0 + i] = CreatePipeline(drawVert, drawFrag, 1, VK_FORMAT_R8G8B8A8_UNORM, (int)i);
    }
    V.pipelines[PIPE_PAINT] = CreatePipeline(quadVert, paintFrag, 0, VK_FORMAT_R8G8B8A8_UNORM, -1);
    V.pipelines[PIPE_DOWNSAMPLE] = CreatePipeline(quadVert, downsampleFrag, 0, VK_FORMAT_R8G8B8A8_UNORM, -1);
    V.pipelines[PIPE_PRESENT] = CreatePipeline(quadVert, presentFrag, 0, V.swapFormat, -1);
    V.pipelines[PIPE_SHOWN] = CreatePipeline(quadVert, presentFrag, 0, VK_FORMAT_R8G8B8A8_UNORM, -1);
    V.pipelines[PIPE_OVERLAY] = CreatePipeline(quadVert, overlayFrag, 0, V.swapFormat, 4);
    vkDestroyShaderModule(V.device, drawVert, NULL);
    vkDestroyShaderModule(V.device, drawFrag, NULL);
    vkDestroyShaderModule(V.device, quadVert, NULL);
    vkDestroyShaderModule(V.device, paintFrag, NULL);
    vkDestroyShaderModule(V.device, downsampleFrag, NULL);
    vkDestroyShaderModule(V.device, presentFrag, NULL);
    vkDestroyShaderModule(V.device, overlayFrag, NULL);

    /* Everything into the GENERAL layout, the surfaces black. */
    BeginCommands();
    memset(&black, 0, sizeof(black));
    ImageLayout(V.vram.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
    ImageLayout(V.display24.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
    ImageLayout(V.overlay.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
    ImageLayout(V.native.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
    for (i = 0; i < SURFACE_COUNT; i++) {
        VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        ImageLayout(V.surfaces[i].image.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
        vkCmdClearColorImage(V.cmd, V.surfaces[i].image.image, VK_IMAGE_LAYOUT_GENERAL, &black, 1, &range);
    }
    Barrier();
    SubmitAndWait(VK_NULL_HANDLE, VK_NULL_HANDLE);
    UploadVram(0, 0, VRAM_WIDTH, VRAM_HEIGHT);
    GpuSetRenderer(&sRenderer);
    return 1;
}

/* The source of the shown picture: a surface region, or the 24-bit display converted on the CPU. */
static VkDescriptorSet DisplaySource(float uv[4], int *pixelW, int *pixelH, Image **image, int *offX, int *offY,
                                     float *widthRatio)
{
    const GpuDisplay *d = &g_gpuDisplay;
    int w = d->width > 0 ? d->width : 640, h = d->height > 0 ? d->height : 240;
    int i;

    if (w > VRAM_WIDTH) w = VRAM_WIDTH;
    if (h > VRAM_HEIGHT) h = VRAM_HEIGHT;
    if (d->rgb24) {
        /* A 24-bit picture (the movies) is bytes, not pixels, in video memory: converted here. */
        uint32_t bytes = (uint32_t)(w * h * 4);
        uint32_t *dst;
        VkBufferImageCopy copy;
        int x, y;
        SyncRect(d->x, d->y, (w * 3 + 1) / 2, h);
        if (V.uploadUsed + bytes > V.uploads.size) {
            SubmitAndWait(VK_NULL_HANDLE, VK_NULL_HANDLE);
        }
        BeginCommands();
        EndRendering();
        dst = (uint32_t *)(V.uploads.mapped + V.uploadUsed);
        for (y = 0; y < h; y++) {
            const uint8_t *row = (const uint8_t *)g_vram[(d->y + y) & 511];
            for (x = 0; x < w; x++) {
                int b = (d->x * 2 + x * 3) & 2047;
                dst[y * w + x] = row[b] | (uint32_t)row[(b + 1) & 2047] << 8 | (uint32_t)row[(b + 2) & 2047] << 16 |
                                 0xFF000000u;
            }
        }
        memset(&copy, 0, sizeof(copy));
        copy.bufferOffset = V.uploadUsed;
        copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        copy.imageSubresource.layerCount = 1;
        copy.imageExtent.width = (uint32_t)w;
        copy.imageExtent.height = (uint32_t)(h > 480 ? 480 : h);
        copy.imageExtent.depth = 1;
        vkCmdCopyBufferToImage(V.cmd, V.uploads.buffer, V.display24.image, VK_IMAGE_LAYOUT_GENERAL, 1, &copy);
        Barrier();
        V.uploadUsed += bytes;
        uv[0] = uv[1] = 0;
        uv[2] = (float)w / V.display24.width;
        uv[3] = (float)h / V.display24.height;
        *pixelW = w, *pixelH = h, *image = &V.display24, *offX = 0, *offY = 0;
        *widthRatio = 1.0f;
        return V.display24Set;
    }
    for (i = SURFACE_FB0; i <= SURFACE_FB1; i++) {
        Surface *s = &V.surfaces[i];
        if (d->x == s->x && d->y == s->y && w == s->w && h == s->h) {
            break;
        }
    }
    if (i > SURFACE_FB1) {
        i = SURFACE_VRAM;
    }
    {
        Surface *s = &V.surfaces[i];
        /* A display buffer is shown with its margins. */
        int left = d->x - s->margin, width = w + 2 * s->margin;
        uv[0] = (float)(left - SurfaceLeft(s)) / SurfaceWidth(s);
        uv[1] = (float)(d->y - s->y) / s->h;
        uv[2] = (float)(left - SurfaceLeft(s) + width) / SurfaceWidth(s);
        uv[3] = (float)(d->y - s->y + h) / s->h;
        *pixelW = width * V.scale, *pixelH = h * V.scale, *image = &s->image;
        *offX = (left - SurfaceLeft(s)) * V.scale, *offY = (d->y - s->y) * V.scale;
        *widthRatio = (float)width / w;
        return s->set;
    }
}

/* The shown picture's source for the display mode: DisplaySource's, or for the modes that start from the
 * console's resolution, its copy brought down to it. */
static VkDescriptorSet PrepareDisplay(float uv[4], float *widthRatio, int *mode)
{
    int pixelW, pixelH, offX, offY, scale, w, h;
    Image *image;
    VkDescriptorSet set = DisplaySource(uv, &pixelW, &pixelH, &image, &offX, &offY, widthRatio);
    VkRenderingAttachmentInfo color = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    VkRenderingInfo info = {VK_STRUCTURE_TYPE_RENDERING_INFO};
    VkViewport viewport;
    VkRect2D scissor;
    Push push;

    *mode = V.crt ? DISPLAY_SCANLINES + V.crt - 1 : V.downsample;
    if (*mode < DISPLAY_SHARP) {
        return set;
    }
    scale = image == &V.display24 ? 1 : V.scale;
    w = pixelW / scale, h = pixelH / scale;
    if (w > NATIVE_WIDTH) w = NATIVE_WIDTH;
    if (h > NATIVE_HEIGHT) h = NATIVE_HEIGHT;
    if (w <= 0 || h <= 0) {
        *mode = DISPLAY_BILINEAR;
        return set;
    }
    color.imageView = V.native.view;
    color.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    info.renderArea.extent.width = (uint32_t)w;
    info.renderArea.extent.height = (uint32_t)h;
    info.layerCount = 1;
    info.colorAttachmentCount = 1;
    info.pColorAttachments = &color;
    vkCmdBeginRendering(V.cmd, &info);
    viewport.x = viewport.y = 0;
    viewport.width = (float)w;
    viewport.height = (float)h;
    viewport.minDepth = 0;
    viewport.maxDepth = 1;
    scissor.offset.x = scissor.offset.y = 0;
    scissor.extent = info.renderArea.extent;
    vkCmdSetViewport(V.cmd, 0, 1, &viewport);
    vkCmdSetScissor(V.cmd, 0, 1, &scissor);
    vkCmdBindPipeline(V.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, V.pipelines[PIPE_DOWNSAMPLE]);
    vkCmdBindDescriptorSets(V.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, V.layout, 0, 1, &set, 0, NULL);
    memset(&push, 0, sizeof(push));
    push.rect[0] = -1, push.rect[1] = -1, push.rect[2] = 1, push.rect[3] = 1;
    push.extra[0] = (float)offX, push.extra[1] = (float)offY, push.extra[2] = (float)scale;
    PushConstants(&push);
    vkCmdDraw(V.cmd, 4, 1, 0, 0);
    vkCmdEndRendering(V.cmd);
    Barrier();
    V.boundPipe = -1;
    uv[0] = uv[1] = 0;
    uv[2] = (float)w / NATIVE_WIDTH;
    uv[3] = (float)h / NATIVE_HEIGHT;
    return V.nativeSet;
}

/* Draws the prepared picture at its aspect ratio, centred in a target of width x height, with bars where
 * the target's shape differs. Inside a rendering pass. */
static void DrawPicture(int pipe, VkDescriptorSet set, const float uv[4], int mode, float aspect, uint32_t width,
                        uint32_t height)
{
    VkViewport viewport;
    VkRect2D scissor;
    Push push;
    float w = (float)width, h = w / aspect;
    if (h > height) {
        h = (float)height;
        w = h * aspect;
    }
    viewport.x = (width - w) / 2;
    viewport.y = (height - h) / 2;
    viewport.width = w;
    viewport.height = h;
    viewport.minDepth = 0;
    viewport.maxDepth = 1;
    scissor.offset.x = scissor.offset.y = 0;
    scissor.extent.width = width;
    scissor.extent.height = height;
    vkCmdSetViewport(V.cmd, 0, 1, &viewport);
    vkCmdSetScissor(V.cmd, 0, 1, &scissor);
    vkCmdBindPipeline(V.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, V.pipelines[pipe]);
    vkCmdBindDescriptorSets(V.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, V.layout, 0, 1, &set, 0, NULL);
    push.rect[0] = -1, push.rect[1] = -1, push.rect[2] = 1, push.rect[3] = 1;
    memcpy(push.extra, uv, sizeof(push.extra));
    push.params[0] = (float)mode;
    push.params[1] = (float)V.curvature;
    push.params[2] = w, push.params[3] = h;
    PushConstants(&push);
    vkCmdDraw(V.cmd, 4, 1, 0, 0);
}

void VkRendererSetDisplay(int downsample, int crt, int curvature)
{
    V.downsample = downsample < 0 ? 0 : (downsample > DISPLAY_SHARP ? DISPLAY_SHARP : downsample);
    V.crt = crt < 0 ? 0 : (crt > DISPLAY_MASK - DISPLAY_SCANLINES + 1 ? DISPLAY_MASK - DISPLAY_SCANLINES + 1 : crt);
    V.curvature = curvature ? 1 : 0;
}

/* Copies a changed overlay picture to its image. */
static void UploadOverlay(void)
{
    uint32_t bytes = OVERLAY_WIDTH * OVERLAY_HEIGHT * 4;
    VkBufferImageCopy copy;
    if (!V.overlayPixels || !V.overlayDirty) {
        return;
    }
    if (V.uploadUsed + bytes > V.uploads.size) {
        SubmitAndWait(VK_NULL_HANDLE, VK_NULL_HANDLE);
    }
    BeginCommands();
    EndRendering();
    memcpy(V.uploads.mapped + V.uploadUsed, V.overlayPixels, bytes);
    memset(&copy, 0, sizeof(copy));
    copy.bufferOffset = V.uploadUsed;
    copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    copy.imageSubresource.layerCount = 1;
    copy.imageExtent.width = OVERLAY_WIDTH;
    copy.imageExtent.height = OVERLAY_HEIGHT;
    copy.imageExtent.depth = 1;
    vkCmdCopyBufferToImage(V.cmd, V.uploads.buffer, V.overlay.image, VK_IMAGE_LAYOUT_GENERAL, 1, &copy);
    Barrier();
    V.uploadUsed += bytes;
    V.overlayDirty = 0;
}

void VkRendererSetOverlay(const uint32_t *rgba, int changed)
{
    V.overlayPixels = rgba;
    if (changed) {
        V.overlayDirty = 1;
    }
}

void VkRendererSetVsync(int on)
{
    if (V.vsync == !!on) {
        return;
    }
    V.vsync = !!on;
    if (V.device) {
        SubmitAndWait(VK_NULL_HANDLE, VK_NULL_HANDLE);
        vkDeviceWaitIdle(V.device);
        CreateSwapchain();
    }
}

static void DestroyImage(Image *img)
{
    vkDestroyImageView(V.device, img->view, NULL);
    vkDestroyImage(V.device, img->image, NULL);
    vkFreeMemory(V.device, img->memory, NULL);
    memset(img, 0, sizeof(*img));
}

void VkRendererSetScale(int scale)
{
    int i;
    VkClearColorValue black;
    scale = scale < 1 ? 1 : (scale > 8 ? 8 : scale);
    if (!V.device || scale == V.scale) {
        return;
    }
    /* What was drawn goes back to video memory first; the new surfaces start from it, at the new scale. */
    for (i = 0; i < SURFACE_COUNT; i++) {
        SyncSurface(i);
    }
    SubmitAndWait(VK_NULL_HANDLE, VK_NULL_HANDLE);
    vkDeviceWaitIdle(V.device);
    V.scale = scale;
    for (i = 0; i < SURFACE_COUNT; i++) {
        Surface *s = &V.surfaces[i];
        DestroyImage(&s->image);
        CreateImage(&s->image, SurfaceWidth(s) * V.scale, s->h * V.scale, VK_FORMAT_R8G8B8A8_UNORM,
                    VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                        VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
        WriteDescriptor(s->set, s->image.view);
        s->dirty = 0;
    }
    vkDestroyBuffer(V.device, V.readback.buffer, NULL);
    vkFreeMemory(V.device, V.readback.memory, NULL);
    CreateBuffer(&V.readback, (uint32_t)((VRAM_WIDTH + 2 * V.margin) * V.scale) * (uint32_t)(VRAM_HEIGHT * V.scale) * 4,
                 VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    BeginCommands();
    memset(&black, 0, sizeof(black));
    for (i = 0; i < SURFACE_COUNT; i++) {
        VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        ImageLayout(V.surfaces[i].image.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
        vkCmdClearColorImage(V.cmd, V.surfaces[i].image.image, VK_IMAGE_LAYOUT_GENERAL, &black, 1, &range);
    }
    Barrier();
    PaintSurfaces(0, 0, VRAM_WIDTH, VRAM_HEIGHT);
    SubmitAndWait(VK_NULL_HANDLE, VK_NULL_HANDLE);
}

void VkRendererPresent(float aspect)
{
    float widthRatio;
    uint32_t index;
    VkResult result;
    float uv[4];
    int mode;
    VkDescriptorSet set;
    VkRenderingAttachmentInfo color = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    VkRenderingInfo info = {VK_STRUCTURE_TYPE_RENDERING_INFO};
    VkViewport viewport;
    VkRect2D scissor;
    Push push;
    VkPresentInfoKHR present = {VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    float w, h;

    if (SDL_GetWindowFlags(V.window) & SDL_WINDOW_HIDDEN) {
        SubmitAndWait(VK_NULL_HANDLE, VK_NULL_HANDLE); /* a test's hidden window: drawn, never shown */
        return;
    }
    if (!V.swapchain || V.swapExtent.width == 0) {
        CreateSwapchain();
        if (!V.swapchain || V.swapExtent.width == 0) {
            SubmitAndWait(VK_NULL_HANDLE, VK_NULL_HANDLE);
            return;
        }
    }
    BeginCommands();
    EndRendering();
    set = PrepareDisplay(uv, &widthRatio, &mode);
    aspect *= widthRatio;
    UploadOverlay();
    result = vkAcquireNextImageKHR(V.device, V.swapchain, UINT64_MAX, V.acquired, VK_NULL_HANDLE, &index);
    if (result == VK_ERROR_OUT_OF_DATE_KHR) {
        SubmitAndWait(VK_NULL_HANDLE, VK_NULL_HANDLE);
        vkDeviceWaitIdle(V.device);
        CreateSwapchain();
        return;
    }
    ImageLayout(V.swapImages[index], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    color.imageView = V.swapViews[index];
    color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    info.renderArea.extent = V.swapExtent;
    info.layerCount = 1;
    info.colorAttachmentCount = 1;
    info.pColorAttachments = &color;
    vkCmdBeginRendering(V.cmd, &info);
    if (g_gpuDisplay.enabled) {
        DrawPicture(PIPE_PRESENT, set, uv, mode, aspect, V.swapExtent.width, V.swapExtent.height);
    }
    if (V.overlayPixels) {
        /* The overlay at 16:9, centred over the whole window. */
        w = (float)V.swapExtent.width;
        h = w * OVERLAY_HEIGHT / OVERLAY_WIDTH;
        if (h > V.swapExtent.height) {
            h = (float)V.swapExtent.height;
            w = h * OVERLAY_WIDTH / OVERLAY_HEIGHT;
        }
        viewport.x = (V.swapExtent.width - w) / 2;
        viewport.y = (V.swapExtent.height - h) / 2;
        viewport.width = w;
        viewport.height = h;
        viewport.minDepth = 0;
        viewport.maxDepth = 1;
        scissor.offset.x = scissor.offset.y = 0;
        scissor.extent = V.swapExtent;
        vkCmdSetViewport(V.cmd, 0, 1, &viewport);
        vkCmdSetScissor(V.cmd, 0, 1, &scissor);
        vkCmdBindPipeline(V.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, V.pipelines[PIPE_OVERLAY]);
        vkCmdBindDescriptorSets(V.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, V.layout, 0, 1, &V.overlaySet, 0, NULL);
        memset(&push, 0, sizeof(push));
        push.rect[0] = -1, push.rect[1] = -1, push.rect[2] = 1, push.rect[3] = 1;
        push.extra[0] = push.extra[1] = 0, push.extra[2] = push.extra[3] = 1;
        PushConstants(&push);
        vkCmdDraw(V.cmd, 4, 1, 0, 0);
    }
    vkCmdEndRendering(V.cmd);
    ImageLayout(V.swapImages[index], VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    V.boundPipe = -1;
    SubmitAndWait(V.acquired, V.rendered);
    present.waitSemaphoreCount = 1;
    present.pWaitSemaphores = &V.rendered;
    present.swapchainCount = 1;
    present.pSwapchains = &V.swapchain;
    present.pImageIndices = &index;
    result = vkQueuePresentKHR(V.queue, &present);
    if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR) {
        vkDeviceWaitIdle(V.device);
        CreateSwapchain();
    }
}

const uint32_t *VkRendererCaptureShown(float aspect, int width, int height)
{
    VkRenderingAttachmentInfo color = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    VkRenderingInfo info = {VK_STRUCTURE_TYPE_RENDERING_INFO};
    VkBufferImageCopy copy;
    VkDescriptorSet set;
    static uint32_t *capture;
    const uint32_t *src;
    float uv[4], widthRatio;
    int mode, i;

    if ((uint32_t)width * (uint32_t)height * 4 > V.readback.size) {
        return NULL;
    }
    BeginCommands();
    EndRendering();
    if (V.shown.width != width || V.shown.height != height) {
        SubmitAndWait(VK_NULL_HANDLE, VK_NULL_HANDLE);
        if (V.shown.image) {
            vkDeviceWaitIdle(V.device);
            vkDestroyImageView(V.device, V.shown.view, NULL);
            vkDestroyImage(V.device, V.shown.image, NULL);
            vkFreeMemory(V.device, V.shown.memory, NULL);
        }
        CreateImage(&V.shown, width, height, VK_FORMAT_R8G8B8A8_UNORM,
                    VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
        BeginCommands();
        ImageLayout(V.shown.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
    }
    set = PrepareDisplay(uv, &widthRatio, &mode);
    color.imageView = V.shown.view;
    color.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    info.renderArea.extent.width = (uint32_t)width;
    info.renderArea.extent.height = (uint32_t)height;
    info.layerCount = 1;
    info.colorAttachmentCount = 1;
    info.pColorAttachments = &color;
    vkCmdBeginRendering(V.cmd, &info);
    if (g_gpuDisplay.enabled) {
        DrawPicture(PIPE_SHOWN, set, uv, mode, aspect * widthRatio, (uint32_t)width, (uint32_t)height);
    }
    vkCmdEndRendering(V.cmd);
    Barrier();
    V.boundPipe = -1;
    memset(&copy, 0, sizeof(copy));
    copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    copy.imageSubresource.layerCount = 1;
    copy.imageExtent.width = (uint32_t)width;
    copy.imageExtent.height = (uint32_t)height;
    copy.imageExtent.depth = 1;
    vkCmdCopyImageToBuffer(V.cmd, V.shown.image, VK_IMAGE_LAYOUT_GENERAL, V.readback.buffer, 1, &copy);
    Barrier();
    SubmitAndWait(VK_NULL_HANDLE, VK_NULL_HANDLE);
    capture = SDL_realloc(capture, (size_t)width * height * 4);
    src = (const uint32_t *)V.readback.mapped;
    for (i = 0; i < width * height; i++) {
        uint32_t p = src[i]; /* RGBA bytes: R in the low byte */
        capture[i] = (p & 0xFF) << 16 | (p & 0xFF00) | ((p >> 16) & 0xFF);
    }
    return capture;
}

const uint32_t *VkRendererCapture(int *width, int *height)
{
    float uv[4];
    int pixelW, pixelH, offX, offY, i;
    float widthRatio;
    Image *image;
    VkBufferImageCopy copy;
    const uint32_t *src;
    static uint32_t *capture;

    BeginCommands();
    EndRendering();
    DisplaySource(uv, &pixelW, &pixelH, &image, &offX, &offY, &widthRatio);
    if (offX + pixelW > image->width) pixelW = image->width - offX;
    if (offY + pixelH > image->height) pixelH = image->height - offY;
    memset(&copy, 0, sizeof(copy));
    copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    copy.imageSubresource.layerCount = 1;
    copy.imageOffset.x = offX;
    copy.imageOffset.y = offY;
    copy.imageExtent.width = (uint32_t)pixelW;
    copy.imageExtent.height = (uint32_t)pixelH;
    copy.imageExtent.depth = 1;
    vkCmdCopyImageToBuffer(V.cmd, image->image, VK_IMAGE_LAYOUT_GENERAL, V.readback.buffer, 1, &copy);
    Barrier();
    SubmitAndWait(VK_NULL_HANDLE, VK_NULL_HANDLE);
    capture = SDL_realloc(capture, (size_t)pixelW * pixelH * 4);
    src = (const uint32_t *)V.readback.mapped;
    for (i = 0; i < pixelW * pixelH; i++) {
        uint32_t p = src[i]; /* RGBA bytes: R in the low byte */
        capture[i] = (p & 0xFF) << 16 | (p & 0xFF00) | ((p >> 16) & 0xFF);
    }
    *width = pixelW;
    *height = pixelH;
    return capture;
}

void VkRendererShutdown(void)
{
    if (V.device) {
        vkDeviceWaitIdle(V.device);
    }
}
