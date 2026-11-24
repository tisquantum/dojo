/* The player's settings: dojo.ini beside the executable, changed in the options overlay (overlay.c).
 * Environment variables (DOJO_FPS and the rest) override the file for a session. */

#ifndef PORT_SETTINGS_H
#define PORT_SETTINGS_H

typedef struct Settings {
    int fps;        /* the simulation's rate: 0 follows the display, else a multiple of 30 up to 480 */
    int scale;      /* the Vulkan renderer's resolution, times the console's: 1 to 8 */
    int aspect;     /* an index into SETTINGS_ASPECTS */
    int software;   /* 1: the CPU renderer */
    int fullscreen;
    int vsync;
    int volume;     /* percent */
    int showFps;    /* the frame counter */
    int analog;     /* the left stick works the d-pad */
    char name[24];  /* the player's name online */
    char server[128]; /* the matchmaking server, host:port */
    int p2pPort;    /* the UDP port a player-to-player match is hosted on */
    int watch;      /* King of the Hill: watch the match while waiting in line */
    int offline;    /* never contact the matchmaking server (direct player-to-player matches still work) */
} Settings;

#define SETTINGS_ASPECT_COUNT 7
#define SETTINGS_ASPECT_AUTO 0 /* the display's own shape */
#define SETTINGS_ASPECT_16_9 3
extern const char *const SETTINGS_ASPECTS[SETTINGS_ASPECT_COUNT]; /* "auto", "4:3", "16:9", ... */

extern Settings g_settings;

/* Loads dojo.ini (missing values keep their defaults); a scripted run (testing) keeps the defaults. */
void SettingsLoad(int scripted);
void SettingsSave(void);

#endif
