/* The player's settings (settings.h): a small key = value file beside the executable. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <SDL3/SDL.h>
#include "settings.h"
#include "input.h"

const char *const SETTINGS_ASPECTS[SETTINGS_ASPECT_COUNT] = {"auto", "4:3", "16:10", "16:9", "21:9", "24:10", "32:9"};

Settings g_settings = {0, 4, SETTINGS_ASPECT_AUTO, 0, 0, 0, 100, 0, 0, 50, 100, 1, 0, {"auto", "auto"}, {"", ""}, {{0}},
                       "", "dojo-matchmaker.fly.dev:7777", 7778, 1, 0, -1, 160, 0};

static int sScripted;

static const struct {
    const char *key;
    int *value;
} sKeys[] = {
    {"fps", &g_settings.fps},           {"scale", &g_settings.scale},   {"software", &g_settings.software},
    {"fullscreen", &g_settings.fullscreen}, {"vsync", &g_settings.vsync}, {"volume", &g_settings.volume},
    {"show_fps", &g_settings.showFps},  {"analog_stick", &g_settings.analog}, {"p2p_port", &g_settings.p2pPort},
    {"watch_hill", &g_settings.watch},  {"offline", &g_settings.offline}, {"stick_deadzone", &g_settings.deadzone},
    {"rumble", &g_settings.rumble},     {"dualsense_haptics", &g_settings.haptics},
    {"online_delay", &g_settings.onlineDelay},  {"quick_max_ping", &g_settings.quickMaxPing},
    {"online_first_to", &g_settings.firstTo},
};

/* The settings that are text. */
static const struct {
    const char *key;
    char *value;
    size_t size;
} sTexts[] = {
    {"name", g_settings.name, sizeof(g_settings.name)},
    {"server", g_settings.server, sizeof(g_settings.server)},
    {"pad1", g_settings.pad[0], sizeof(g_settings.pad[0])},
    {"pad2", g_settings.pad[1], sizeof(g_settings.pad[1])},
    {"pad1_name", g_settings.padName[0], sizeof(g_settings.padName[0])},
    {"pad2_name", g_settings.padName[1], sizeof(g_settings.padName[1])},
};

static void DefaultName(void)
{
    const char *user = SDL_getenv("USERNAME");
    size_t i, n = 0;
    for (i = 0; user && user[i] && n < sizeof(g_settings.name) - 1; i++) {
        char c = user[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-') {
            g_settings.name[n++] = c;
        }
    }
    g_settings.name[n] = 0;
    if (n == 0) {
        snprintf(g_settings.name, sizeof(g_settings.name), "Samurai");
    }
}

static char *Path(void)
{
    static char path[1024];
    const char *base = SDL_GetBasePath();
    snprintf(path, sizeof(path), "%sdojo.ini", base ? base : "");
    return path;
}

static int Clamp(int v, int lo, int hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

void SettingsLoad(int scripted)
{
    FILE *f;
    char line[256];
    sScripted = scripted;
    DefaultName();
    InputDefaultBindings(-1);
    if (scripted) {
        g_settings.aspect = SETTINGS_ASPECT_16_9; /* a test's picture doesn't depend on the monitor */
    }
    if (scripted || !(f = fopen(Path(), "r"))) {
        return;
    }
    while (fgets(line, sizeof(line), f)) {
        char key[64], value[128];
        size_t i, n;
        int found = 0;
        if (sscanf(line, " %63[^= ] = %127[^\r\n]", key, value) != 2) {
            continue;
        }
        for (n = strlen(value); n > 0 && value[n - 1] == ' '; n--) {
            value[n - 1] = 0;
        }
        for (i = 0; i < sizeof(sTexts) / sizeof(sTexts[0]); i++) {
            if (strcmp(key, sTexts[i].key) == 0) {
                snprintf(sTexts[i].value, sTexts[i].size, "%s", value);
                found = 1;
            }
        }
        if (found) {
            continue;
        }
        if (strcmp(key, "keyboard_player") == 0) {
            g_settings.keyboardPlayer = atoi(value) == 2;
            continue;
        }
        if (strncmp(key, "bind_", 5) == 0) {
            /* four bindings in hex (input.h), 0 for none */
            for (i = 0; i < SETTINGS_BUTTONS; i++) {
                int *b = g_settings.binds[i];
                if (strcmp(key + 5, INPUT_BUTTONS[i].key) == 0 &&
                    sscanf(value, "%x %x %x %x", (unsigned *)&b[0], (unsigned *)&b[1], (unsigned *)&b[2],
                           (unsigned *)&b[3]) != 4) {
                    InputDefaultBindings((int)i);
                }
            }
            continue;
        }
        if (strcmp(key, "aspect") == 0) {
            for (i = 0; i < SETTINGS_ASPECT_COUNT; i++) {
                if (strcmp(value, SETTINGS_ASPECTS[i]) == 0) {
                    g_settings.aspect = (int)i;
                }
            }
            continue;
        }
        for (i = 0; i < sizeof(sKeys) / sizeof(sKeys[0]); i++) {
            if (strcmp(key, sKeys[i].key) == 0) {
                *sKeys[i].value = atoi(value);
            }
        }
    }
    fclose(f);
    g_settings.fps = g_settings.fps <= 0 ? 0 : Clamp((g_settings.fps + 15) / 30 * 30, 30, 480);
    g_settings.scale = Clamp(g_settings.scale, 1, 8);
    g_settings.volume = Clamp(g_settings.volume, 0, 100);
    g_settings.deadzone = Clamp(g_settings.deadzone, 10, 90);
    g_settings.rumble = Clamp(g_settings.rumble, 0, 100);
    g_settings.onlineDelay = Clamp(g_settings.onlineDelay, -1, 8);
    g_settings.quickMaxPing = Clamp(g_settings.quickMaxPing, 0, 1000);
    g_settings.firstTo = Clamp(g_settings.firstTo, 0, 9);
}

void SettingsSave(void)
{
    FILE *f;
    size_t i;
    if (sScripted || !(f = fopen(Path(), "w"))) {
        return;
    }
    fprintf(f, "; Dojo: the options menu (F1) writes this file.\n");
    fprintf(f, "aspect = %s\n", SETTINGS_ASPECTS[g_settings.aspect]);
    for (i = 0; i < sizeof(sTexts) / sizeof(sTexts[0]); i++) {
        fprintf(f, "%s = %s\n", sTexts[i].key, sTexts[i].value);
    }
    for (i = 0; i < sizeof(sKeys) / sizeof(sKeys[0]); i++) {
        fprintf(f, "%s = %d\n", sKeys[i].key, *sKeys[i].value);
    }
    fprintf(f, "keyboard_player = %d\n", g_settings.keyboardPlayer + 1);
    fprintf(f, "; Controls: two gamepad bindings, then two keyboard-and-mouse ones (port/src/input.h), 0 for none.\n");
    for (i = 0; i < SETTINGS_BUTTONS; i++) {
        const int *b = g_settings.binds[i];
        fprintf(f, "bind_%s = %x %x %x %x\n", INPUT_BUTTONS[i].key, b[0], b[1], b[2], b[3]);
    }
    fclose(f);
}
