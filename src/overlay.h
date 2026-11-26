/* The options overlay: a menu over the game (F1, Esc or a gamepad's guide button) for the settings in
 * settings.h, and the frame counter. Offline the game waits while it is open; online it runs on, with this
 * machine's controller let go. */

#ifndef PORT_OVERLAY_H
#define PORT_OVERLAY_H

#include <stdint.h>
#include <SDL3/SDL.h>

#define OVERLAY_WIDTH  640
#define OVERLAY_HEIGHT 360

/* What the host is running, for what the menu may change and what waits for a restart. */
typedef struct OverlayContext {
    int vulkan;         /* the Vulkan renderer is running (scale and vsync apply) */
    int online;         /* in an online match: the frame rate is the match's */
    int runningAspect;  /* SETTINGS_ASPECTS index the session started with */
    int displayW, displayH; /* the display an "auto" aspect follows, if the session runs with one */
    int runningSoftware;
    int displayHz;      /* the display's refresh, for "auto" */
    int ticksPerFrame;  /* the simulation's current rate */
} OverlayContext;

/* A setting changed in the menu, for the host to apply. */
typedef enum OverlayChange {
    OVERLAY_CHANGE_FPS,
    OVERLAY_CHANGE_SCALE,
    OVERLAY_CHANGE_FULLSCREEN,
    OVERLAY_CHANGE_VSYNC,
    OVERLAY_CHANGE_VOLUME,
    OVERLAY_CHANGE_RULES,
    OVERLAY_CHANGE_PICTURE, /* the downsampling or the CRT look */
    OVERLAY_CHANGE_QUIT,
} OverlayChange;

void OverlayInit(const OverlayContext *context, void (*apply)(OverlayChange change));
void OverlayUpdateContext(const OverlayContext *context);
/* Takes an event if the overlay wants it (its keys, and everything while it is open); returns 1 if taken. */
int OverlayHandleEvent(const SDL_Event *e);
int OverlayIsOpen(void);
void OverlayOpen(void);
/* Counts a presented picture, for the frame counter. */
void OverlayFramePresented(void);
/* The overlay's picture, OVERLAY_WIDTH x OVERLAY_HEIGHT RGBA bytes (R first), or NULL when there is nothing
 * to show; *changed is 1 when it differs from the last call's. */
const uint32_t *OverlayPicture(int *changed);

#endif
