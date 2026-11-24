/* The player's settings: dojo.ini beside the executable, changed in the options overlay (overlay.c).
 * Environment variables (DOJO_FPS and the rest) override the file for a session. */

#ifndef PORT_SETTINGS_H
#define PORT_SETTINGS_H

#define SETTINGS_BUTTONS    14 /* the console's buttons, in input.c's INPUT_BUTTONS order */
#define SETTINGS_BIND_SLOTS 4  /* two gamepad bindings, then two keyboard-and-mouse ones */

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
    int deadzone;   /* how far, in percent, a stick goes before it counts as a direction */
    int rumble;     /* percent; 0 is off */
    int haptics;    /* a wired DualSense rumbles with its haptic actuators */
    int keyboardPlayer; /* 0 or 1: the player the keyboard and mouse belong to */
    char pad[2][40];    /* each player's gamepad: "auto", "none" or an SDL GUID */
    char padName[2][64]; /* its name, for showing while it is unplugged */
    int binds[SETTINGS_BUTTONS][SETTINGS_BIND_SLOTS]; /* input.h's bindings for each console button */
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
