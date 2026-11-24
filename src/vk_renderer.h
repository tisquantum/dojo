/* The Vulkan renderer: the console GPU's primitives drawn by the graphics card at a multiple of the
 * console's resolution (gpu_renderer.h), and the shown picture presented to the window. */

#ifndef PORT_VK_RENDERER_H
#define PORT_VK_RENDERER_H

#include <stdint.h>
#include <SDL3/SDL.h>

/* Creates the renderer for the window at `scale` times the console's resolution, the display buffers
 * `margin` console pixels wider each side (widescreen); returns 0 if Vulkan is unavailable, and the port
 * then draws in software. */
int VkRendererInit(SDL_Window *window, int scale, int margin);
/* Shows the GPU's display area in the window, `aspect` its width over its height at the console's width
 * (a display buffer's margins widen it). */
void VkRendererPresent(float aspect);
/* The shown picture at the renderer's resolution, as 0x00RRGGBB pixels; the buffer stays valid until the
 * next call. */
const uint32_t *VkRendererCapture(int *width, int *height);
void VkRendererShutdown(void);
/* The options overlay's picture (overlay.h) to lay over the next presents, or NULL; `changed` when its
 * pixels are new. */
void VkRendererSetOverlay(const uint32_t *rgba, int changed);
/* Live settings: the resolution as a multiple of the console's (the surfaces are made again, from video
 * memory), and whether presents wait for the display. */
void VkRendererSetScale(int scale);
void VkRendererSetVsync(int on);

#endif
