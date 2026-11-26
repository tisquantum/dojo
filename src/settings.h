/* The player's settings: dojo.ini beside the executable, changed in the options overlay (overlay.c).
 * Environment variables (DOJO_FPS and the rest) override the file for a session. */

#ifndef PORT_SETTINGS_H
#define PORT_SETTINGS_H

#define SETTINGS_BUTTONS    14 /* the console's buttons, in input.c's INPUT_BUTTONS order */
#define SETTINGS_BIND_SLOTS 4  /* two gamepad bindings, then two keyboard-and-mouse ones */
#define SETTINGS_MENU_COMBO 3  /* the gamepad inputs held together to open and close the menu */

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
    int menuCombo[SETTINGS_MENU_COMBO]; /* gamepad bindings, all held at once open or close the menu; 0 unused */
    char name[24];  /* the player's name online */
    char server[128]; /* the matchmaking server, host:port */
    int p2pPort;    /* the UDP port a player-to-player match is hosted on */
    int watch;      /* King of the Hill: watch the match while waiting in line */
    int offline;    /* never contact the matchmaking server (direct player-to-player matches still work) */
    int onlineDelay; /* online input delay in fields: -1 picks the least the connection allows */
    int quickMaxPing; /* quick play turns down an opponent further than this (ms, 0: any) for its first 30 s */
    int firstTo;    /* an online set's length: the first to win this many bouts (0: no end) */
    /* The rules (pc_rules.h); online, each holds only when both players have it on. */
    int fixBugs;    /* the original game's bugs fixed */
    int extras;     /* what the disc holds that play never reached */
    int jpBlood;    /* the Japanese release's semi-transparent amber blood */
    /* How the picture is shown (the Vulkan renderer's): see SETTINGS_DOWNSAMPLES and SETTINGS_CRTS. */
    int downsample; /* 0 as drawn, 1 smoothed to the window (supersampled), 2 the console's resolution */
    int crt;        /* 0 none, 1 scanlines, 2 aperture grille, 3 shadow mask */
    int crtCurvature; /* the CRT's glass curved */
} Settings;

#define SETTINGS_ASPECT_COUNT 7
#define SETTINGS_ASPECT_AUTO 0 /* the display's own shape */
#define SETTINGS_ASPECT_16_9 3
extern const char *const SETTINGS_ASPECTS[SETTINGS_ASPECT_COUNT]; /* "auto", "4:3", "16:9", ... */
#define SETTINGS_DOWNSAMPLE_COUNT 3
extern const char *const SETTINGS_DOWNSAMPLES[SETTINGS_DOWNSAMPLE_COUNT]; /* "off", "smooth", "native" */
#define SETTINGS_CRT_COUNT 4
extern const char *const SETTINGS_CRTS[SETTINGS_CRT_COUNT]; /* "off", "scanlines", "aperture", "mask" */

extern Settings g_settings;

/* Loads dojo.ini (missing values keep their defaults); a scripted run (testing) keeps the defaults. */
void SettingsLoad(int scripted);
void SettingsSave(void);

#endif
