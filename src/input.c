/* The controls (input.h).
 *
 * Every connected gamepad is open; each player's is chosen in the controls page (auto takes the first free
 * one), and the keyboard and mouse belong to one player. The bindings are the same for both players.
 *
 * Rumble comes from the game's own DualShock pulse (gRumblePulses: a duty, in 4096ths, the console turns into
 * a motor's on and off), as a strength for SDL's rumble. A DualSense on a cable has no motors but haptic
 * actuators, which Windows sees as the third and fourth channels of the controller's sound device; with
 * haptics on, they play the game's sound effects (spu.c: footsteps, swings, clashes, hits, panned as heard):
 * their low end as it is, and their higher sounds, which the actuators can't follow, as taps of a 180 Hz wave
 * that follows their loudness, both only where a sound begins (a fast loudness above a slow one), so a step or
 * a clash is a tap and a held sound fades away. The pulse is a wave on top. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "input.h"
#include "settings.h"
#include "netplay.h"

#define MAX_PADS 8
#define TRIGGER_ON 16000
#define HAPTIC_RATE 48000

const InputButton INPUT_BUTTONS[SETTINGS_BUTTONS] = {
    {"Cross", "cross", 1 << 6},     {"Circle", "circle", 1 << 5},     {"Square", "square", 1 << 7},
    {"Triangle", "triangle", 1 << 4}, {"L1", "l1", 1 << 2},          {"R1", "r1", 1 << 3},
    {"L2", "l2", 1 << 0},           {"R2", "r2", 1 << 1},             {"Start", "start", 1 << 11},
    {"Select", "select", 1 << 8},   {"D-pad up", "up", 1 << 12},      {"D-pad down", "down", 1 << 14},
    {"D-pad left", "left", 1 << 15}, {"D-pad right", "right", 1 << 13},
};

static const int sDefaults[SETTINGS_BUTTONS][SETTINGS_BIND_SLOTS] = {
    {INPUT_BIND_PAD(SDL_GAMEPAD_BUTTON_SOUTH), 0, INPUT_BIND_KEY(SDL_SCANCODE_X), 0},
    {INPUT_BIND_PAD(SDL_GAMEPAD_BUTTON_EAST), 0, INPUT_BIND_KEY(SDL_SCANCODE_C), 0},
    {INPUT_BIND_PAD(SDL_GAMEPAD_BUTTON_WEST), 0, INPUT_BIND_KEY(SDL_SCANCODE_Z), 0},
    {INPUT_BIND_PAD(SDL_GAMEPAD_BUTTON_NORTH), 0, INPUT_BIND_KEY(SDL_SCANCODE_S), 0},
    {INPUT_BIND_PAD(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER), 0, INPUT_BIND_KEY(SDL_SCANCODE_Q), 0},
    {INPUT_BIND_PAD(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER), 0, INPUT_BIND_KEY(SDL_SCANCODE_E), 0},
    {INPUT_BIND_AXIS(SDL_GAMEPAD_AXIS_LEFT_TRIGGER, 0), 0, INPUT_BIND_KEY(SDL_SCANCODE_1), 0},
    {INPUT_BIND_AXIS(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, 0), 0, INPUT_BIND_KEY(SDL_SCANCODE_3), 0},
    {INPUT_BIND_PAD(SDL_GAMEPAD_BUTTON_START), 0, INPUT_BIND_KEY(SDL_SCANCODE_RETURN), 0},
    {INPUT_BIND_PAD(SDL_GAMEPAD_BUTTON_BACK), 0, INPUT_BIND_KEY(SDL_SCANCODE_BACKSPACE), 0},
    {INPUT_BIND_PAD(SDL_GAMEPAD_BUTTON_DPAD_UP), 0, INPUT_BIND_KEY(SDL_SCANCODE_UP), 0},
    {INPUT_BIND_PAD(SDL_GAMEPAD_BUTTON_DPAD_DOWN), 0, INPUT_BIND_KEY(SDL_SCANCODE_DOWN), 0},
    {INPUT_BIND_PAD(SDL_GAMEPAD_BUTTON_DPAD_LEFT), 0, INPUT_BIND_KEY(SDL_SCANCODE_LEFT), 0},
    {INPUT_BIND_PAD(SDL_GAMEPAD_BUTTON_DPAD_RIGHT), 0, INPUT_BIND_KEY(SDL_SCANCODE_RIGHT), 0},
};

/* The game's rumble state (rumble_pulse.h). */
extern struct {
    short dutyStep;
    short elapsedFrames;
    short phase;
} gRumblePulses[2];

/* A wired DualSense's haptics: a wave on its sound device's third and fourth channels, made in SDL's audio
 * thread from the strength the game thread sets. */
#define HAPTIC_RING 8192 /* frames of the effects' wave waiting for the audio thread */

typedef struct Haptics {
    SDL_AudioStream *stream;
    SDL_AudioDeviceID device;
    SDL_AtomicInt strength; /* 0 to 10000 */
    float level, kick, phase;
    float ring[HAPTIC_RING][2];   /* the effects' wave, left and right actuators */
    SDL_AtomicInt written, read;  /* frames put in and taken out, counting on */
} Haptics;

/* Turning the effects into a wave for the actuators: a resampler from 44.1 to 48 kHz and, each side, two
 * low-pass stages, a loudness follower of what's above them and the carrier it plays. */
static struct {
    double position;          /* in the field's samples, of the next output sample */
    float previous[2];        /* the last field's last sample */
    float low[2][2], envelope[2], phase;
    float fast[2], slow[2];   /* the whole sound's loudness, quick and slow to follow: their gap is an onset */
    FILE *wav;                /* DOJO_HAPTICS_WAV */
    uint32_t wavBytes;
} sShaper;

static struct {
    SDL_Gamepad *pad;
    char guid[40];
} sPads[MAX_PADS];
static int sPadCount;
static int sPlayerPad[2] = {-1, -1}; /* an index into sPads, or -1 */
static Haptics sHaptics[2];          /* each player's, while their gamepad is a wired DualSense */
static Uint64 sHapticsRetry;         /* when to look for a DualSense's sound device again */
static int sRumbleSent[2] = {-1, -1};
static Uint64 sRumbleSentAt[2];

void InputDefaultBindings(int button)
{
    int i;
    for (i = 0; i < SETTINGS_BUTTONS; i++) {
        if (button < 0 || button == i) {
            memcpy(g_settings.binds[i], sDefaults[i], sizeof(sDefaults[i]));
        }
    }
}

static int IsChosen(const char *choice)
{
    return strcmp(choice, "auto") != 0 && strcmp(choice, "none") != 0;
}

/* Each player's gamepad: the chosen ones first, then auto takes the first left. */
static void AssignPads(void)
{
    int taken[MAX_PADS] = {0}, p, i;
    for (p = 0; p < 2; p++) {
        sPlayerPad[p] = -1;
        for (i = 0; IsChosen(g_settings.pad[p]) && i < sPadCount && sPlayerPad[p] < 0; i++) {
            if (!taken[i] && SDL_strcasecmp(sPads[i].guid, g_settings.pad[p]) == 0) {
                sPlayerPad[p] = i;
                taken[i] = 1;
            }
        }
    }
    for (p = 0; p < 2; p++) {
        for (i = 0; strcmp(g_settings.pad[p], "auto") == 0 && i < sPadCount && sPlayerPad[p] < 0; i++) {
            if (!taken[i]) {
                sPlayerPad[p] = i;
                taken[i] = 1;
            }
        }
    }
    for (p = 0; p < 2; p++) {
        if (sPlayerPad[p] >= 0) {
            SDL_SetGamepadPlayerIndex(sPads[sPlayerPad[p]].pad, p); /* a DualSense's player lights */
        }
    }
}

static void CloseHaptics(int player)
{
    if (sHaptics[player].stream) {
        SDL_DestroyAudioStream(sHaptics[player].stream);
    }
    memset(&sHaptics[player], 0, sizeof(sHaptics[player]));
}

void InputDevicesChanged(void)
{
    int count = 0, i, p;
    SDL_JoystickID *ids = SDL_GetGamepads(&count);
    for (i = 0; i < sPadCount; i++) {
        SDL_CloseGamepad(sPads[i].pad);
    }
    sPadCount = 0;
    for (i = 0; ids && i < count && sPadCount < MAX_PADS; i++) {
        SDL_Gamepad *pad = SDL_OpenGamepad(ids[i]);
        if (pad) {
            sPads[sPadCount].pad = pad;
            SDL_GUIDToString(SDL_GetGamepadGUIDForID(ids[i]), sPads[sPadCount].guid, sizeof(sPads[0].guid));
            sPadCount++;
        }
    }
    SDL_free(ids);
    AssignPads();
    for (p = 0; p < 2; p++) {
        CloseHaptics(p);
        sRumbleSent[p] = -1;
    }
    sHapticsRetry = 0;
}

void InputInit(void)
{
    InputDevicesChanged();
}

void InputShutdown(void)
{
    int i;
    for (i = 0; i < 2; i++) {
        CloseHaptics(i);
    }
    for (i = 0; i < sPadCount; i++) {
        SDL_RumbleGamepad(sPads[i].pad, 0, 0, 0);
        SDL_CloseGamepad(sPads[i].pad);
    }
    sPadCount = 0;
}

static int BindingHeld(int binding, SDL_Gamepad *pad, const bool *keys, SDL_MouseButtonFlags mouse)
{
    int code = binding & 0xFFFF;
    switch (INPUT_BIND_KIND(binding)) {
    case INPUT_BIND_KEY(0):
        return keys && code < SDL_SCANCODE_COUNT && keys[code];
    case INPUT_BIND_MOUSE(0):
        return keys && (mouse & SDL_BUTTON_MASK(code)) != 0;
    case INPUT_BIND_PAD(0):
        return pad && SDL_GetGamepadButton(pad, (SDL_GamepadButton)code);
    case INPUT_BIND_AXIS(0, 0): {
        SDL_GamepadAxis axis = (SDL_GamepadAxis)(code >> 1);
        int value = pad ? SDL_GetGamepadAxis(pad, axis) : 0;
        int on = axis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER || axis == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER
                     ? TRIGGER_ON
                     : g_settings.deadzone * 32767 / 100;
        return (code & 1) ? value < -on : value > on;
    }
    default:
        return 0;
    }
}

unsigned InputButtons(int player)
{
    SDL_Gamepad *pad = sPlayerPad[player] >= 0 ? sPads[sPlayerPad[player]].pad : NULL;
    const bool *keys = g_settings.keyboardPlayer == player ? SDL_GetKeyboardState(NULL) : NULL;
    SDL_MouseButtonFlags mouse = keys ? SDL_GetMouseState(NULL, NULL) : 0;
    unsigned buttons = 0;
    int i, slot;
    for (i = 0; i < SETTINGS_BUTTONS; i++) {
        for (slot = 0; slot < SETTINGS_BIND_SLOTS; slot++) {
            if (BindingHeld(g_settings.binds[i][slot], pad, keys, mouse)) {
                buttons |= INPUT_BUTTONS[i].mask;
                break;
            }
        }
    }
    if (pad && g_settings.analog) {
        /* The left stick as the d-pad, past the dead zone. */
        int on = g_settings.deadzone * 32767 / 100;
        int x = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTX), y = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTY);
        buttons |= (y < -on ? 1u << 12 : 0) | (x > on ? 1u << 13 : 0) | (y > on ? 1u << 14 : 0) |
                   (x < -on ? 1u << 15 : 0);
    }
    return buttons;
}

int InputBindingFromEvent(const SDL_Event *e, int pad)
{
    if (pad) {
        if (e->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN && e->gbutton.button != SDL_GAMEPAD_BUTTON_GUIDE) {
            return INPUT_BIND_PAD(e->gbutton.button);
        }
        if (e->type == SDL_EVENT_GAMEPAD_AXIS_MOTION && (e->gaxis.value > 24000 || e->gaxis.value < -24000)) {
            int trigger = e->gaxis.axis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER || e->gaxis.axis == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER;
            if (!trigger || e->gaxis.value > 0) {
                return INPUT_BIND_AXIS(e->gaxis.axis, e->gaxis.value < 0);
            }
        }
        return 0;
    }
    if (e->type == SDL_EVENT_KEY_DOWN && !e->key.repeat) {
        return INPUT_BIND_KEY(e->key.scancode);
    }
    if (e->type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
        return INPUT_BIND_MOUSE(e->button.button);
    }
    return 0;
}

/* The gamepad the names follow: player one's, or the first. */
static SDL_GamepadType NamingType(void)
{
    int i = sPlayerPad[0] >= 0 ? sPlayerPad[0] : (sPlayerPad[1] >= 0 ? sPlayerPad[1] : (sPadCount ? 0 : -1));
    return i >= 0 ? SDL_GetGamepadType(sPads[i].pad) : SDL_GAMEPAD_TYPE_STANDARD;
}

static const char *PadButtonName(SDL_GamepadType type, SDL_GamepadButton button)
{
    int ps = type == SDL_GAMEPAD_TYPE_PS3 || type == SDL_GAMEPAD_TYPE_PS4 || type == SDL_GAMEPAD_TYPE_PS5;
    int nintendo = type >= SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO && type <= SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_PAIR;
    static const char *const directions[4] = {"D-pad up", "D-pad down", "D-pad left", "D-pad right"};
    switch (button) {
    case SDL_GAMEPAD_BUTTON_SOUTH:
    case SDL_GAMEPAD_BUTTON_EAST:
    case SDL_GAMEPAD_BUTTON_WEST:
    case SDL_GAMEPAD_BUTTON_NORTH:
        switch (SDL_GetGamepadButtonLabelForType(type, button)) {
        case SDL_GAMEPAD_BUTTON_LABEL_A: return "A";
        case SDL_GAMEPAD_BUTTON_LABEL_B: return "B";
        case SDL_GAMEPAD_BUTTON_LABEL_X: return "X";
        case SDL_GAMEPAD_BUTTON_LABEL_Y: return "Y";
        case SDL_GAMEPAD_BUTTON_LABEL_CROSS: return "Cross";
        case SDL_GAMEPAD_BUTTON_LABEL_CIRCLE: return "Circle";
        case SDL_GAMEPAD_BUTTON_LABEL_SQUARE: return "Square";
        case SDL_GAMEPAD_BUTTON_LABEL_TRIANGLE: return "Triangle";
        default:
            return button == SDL_GAMEPAD_BUTTON_SOUTH ? "South"
                   : button == SDL_GAMEPAD_BUTTON_EAST ? "East"
                   : button == SDL_GAMEPAD_BUTTON_WEST ? "West"
                                                        : "North";
        }
    case SDL_GAMEPAD_BUTTON_BACK:
        return type == SDL_GAMEPAD_TYPE_PS5 ? "Create" : type == SDL_GAMEPAD_TYPE_PS4 ? "Share"
             : type == SDL_GAMEPAD_TYPE_PS3 ? "Select" : nintendo ? "Minus"
             : type == SDL_GAMEPAD_TYPE_XBOXONE ? "View" : "Back";
    case SDL_GAMEPAD_BUTTON_START:
        return ps && type != SDL_GAMEPAD_TYPE_PS3 ? "Options" : nintendo ? "Plus"
             : type == SDL_GAMEPAD_TYPE_XBOXONE ? "Menu" : "Start";
    case SDL_GAMEPAD_BUTTON_GUIDE: return ps ? "PS" : nintendo ? "Home" : "Guide";
    case SDL_GAMEPAD_BUTTON_LEFT_STICK: return ps ? "L3" : "LS press";
    case SDL_GAMEPAD_BUTTON_RIGHT_STICK: return ps ? "R3" : "RS press";
    case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: return ps ? "L1" : nintendo ? "L" : "LB";
    case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: return ps ? "R1" : nintendo ? "R" : "RB";
    case SDL_GAMEPAD_BUTTON_DPAD_UP:
    case SDL_GAMEPAD_BUTTON_DPAD_DOWN:
    case SDL_GAMEPAD_BUTTON_DPAD_LEFT:
    case SDL_GAMEPAD_BUTTON_DPAD_RIGHT:
        return directions[button - SDL_GAMEPAD_BUTTON_DPAD_UP];
    case SDL_GAMEPAD_BUTTON_MISC1: return type == SDL_GAMEPAD_TYPE_PS5 ? "Mic" : nintendo ? "Capture" : "Share";
    case SDL_GAMEPAD_BUTTON_RIGHT_PADDLE1: return "Paddle P1";
    case SDL_GAMEPAD_BUTTON_LEFT_PADDLE1: return "Paddle P3";
    case SDL_GAMEPAD_BUTTON_RIGHT_PADDLE2: return "Paddle P2";
    case SDL_GAMEPAD_BUTTON_LEFT_PADDLE2: return "Paddle P4";
    case SDL_GAMEPAD_BUTTON_TOUCHPAD: return "Touchpad";
    default: return NULL;
    }
}

void InputBindingName(int binding, char *out, size_t size)
{
    SDL_GamepadType type = NamingType();
    int code = binding & 0xFFFF;
    int ps = type == SDL_GAMEPAD_TYPE_PS3 || type == SDL_GAMEPAD_TYPE_PS4 || type == SDL_GAMEPAD_TYPE_PS5;
    int nintendo = type >= SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO && type <= SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_PAIR;
    switch (INPUT_BIND_KIND(binding)) {
    case INPUT_BIND_KEY(0): {
        const char *name = SDL_GetScancodeName((SDL_Scancode)code);
        snprintf(out, size, "%s", name && name[0] ? name : "Key ?");
        return;
    }
    case INPUT_BIND_MOUSE(0):
        if (code == SDL_BUTTON_LEFT) {
            snprintf(out, size, "Left click");
        } else if (code == SDL_BUTTON_RIGHT) {
            snprintf(out, size, "Right click");
        } else if (code == SDL_BUTTON_MIDDLE) {
            snprintf(out, size, "Mid click");
        } else {
            snprintf(out, size, "Mouse %d", code);
        }
        return;
    case INPUT_BIND_PAD(0): {
        const char *name = PadButtonName(type, (SDL_GamepadButton)code);
        if (name) {
            snprintf(out, size, "%s", name);
        } else {
            snprintf(out, size, "Button %d", code);
        }
        return;
    }
    case INPUT_BIND_AXIS(0, 0): {
        static const char *const ways[2][2] = {{"right", "left"}, {"down", "up"}};
        SDL_GamepadAxis axis = (SDL_GamepadAxis)(code >> 1);
        if (axis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER || axis == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) {
            int right = axis == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER;
            snprintf(out, size, "%s", ps ? (right ? "R2" : "L2") : nintendo ? (right ? "ZR" : "ZL")
                                                                             : (right ? "RT" : "LT"));
        } else {
            snprintf(out, size, "%s %s", axis <= SDL_GAMEPAD_AXIS_LEFTY ? "LS" : "RS",
                     ways[axis == SDL_GAMEPAD_AXIS_LEFTY || axis == SDL_GAMEPAD_AXIS_RIGHTY][code & 1]);
        }
        return;
    }
    default:
        snprintf(out, size, "None");
        return;
    }
}

/* Choices: 0 auto, 1 none, 2 and on the connected gamepads. */
static int PadChoice(int player)
{
    if (strcmp(g_settings.pad[player], "auto") == 0) {
        return 0;
    }
    if (strcmp(g_settings.pad[player], "none") == 0) {
        return 1;
    }
    return sPlayerPad[player] >= 0 ? 2 + sPlayerPad[player] : -1;
}

void InputStepPlayerPad(int player, int dir)
{
    int count = 2 + sPadCount, choice = PadChoice(player);
    choice = choice < 0 ? 0 : (choice + (dir ? dir : 1) + count) % count;
    if (choice < 2) {
        snprintf(g_settings.pad[player], sizeof(g_settings.pad[player]), "%s", choice ? "none" : "auto");
        g_settings.padName[player][0] = 0;
    } else {
        snprintf(g_settings.pad[player], sizeof(g_settings.pad[player]), "%s", sPads[choice - 2].guid);
        snprintf(g_settings.padName[player], sizeof(g_settings.padName[player]), "%s",
                 SDL_GetGamepadName(sPads[choice - 2].pad));
    }
    InputDevicesChanged();
}

void InputDescribePlayerPad(int player, char *out, size_t size)
{
    const char *name = sPlayerPad[player] >= 0 ? SDL_GetGamepadName(sPads[sPlayerPad[player]].pad) : NULL;
    if (strcmp(g_settings.pad[player], "none") == 0) {
        snprintf(out, size, "None");
    } else if (strcmp(g_settings.pad[player], "auto") == 0) {
        snprintf(out, size, "Auto: %s", name ? name : "none connected");
    } else if (name) {
        snprintf(out, size, "%s", name);
    } else {
        snprintf(out, size, "%s (not connected)", g_settings.padName[player][0] ? g_settings.padName[player] : "Gamepad");
    }
}

/* -- rumble ------------------------------------------------------------------------------------------------ */

static void SDLCALL FeedHaptics(void *user, SDL_AudioStream *stream, int additional, int total)
{
    Haptics *h = user;
    float target = (float)SDL_GetAtomicInt(&h->strength) / 10000.0f;
    int frames = additional / (int)(4 * sizeof(float));
    unsigned written = (unsigned)SDL_GetAtomicInt(&h->written), read = (unsigned)SDL_GetAtomicInt(&h->read);
    (void)total;
    if (written - read > 2400) {
        read = written - 1200; /* behind by more than 50 ms (a stall): catch up to 25 */
    }
    while (frames > 0) {
        float wave[256 * 4];
        int n = frames > 256 ? 256 : frames, i;
        for (i = 0; i < n; i++) {
            float amplitude, frequency;
            if (target > 0.1f && h->level < 0.05f) {
                h->kick = 1; /* a blow's first moment, harder and lower */
            }
            /* a millisecond to rise, ten to fall */
            h->level += (target - h->level) * (target > h->level ? 0.02f : 0.002f);
            h->kick *= 0.9995f;
            frequency = 170.0f - 90.0f * h->level - 40.0f * h->kick;
            h->phase += 2.0f * (float)SDL_PI_F * frequency / HAPTIC_RATE;
            if (h->phase > 2.0f * (float)SDL_PI_F) {
                h->phase -= 2.0f * (float)SDL_PI_F;
            }
            amplitude = h->level * 0.8f + h->kick * h->level * 0.5f;
            amplitude = amplitude > 1 ? 1 : amplitude;
            {
                float pulse = amplitude * SDL_sinf(h->phase), effectL = 0, effectR = 0, l, r;
                if (read != written) {
                    effectL = h->ring[read % HAPTIC_RING][0];
                    effectR = h->ring[read % HAPTIC_RING][1];
                    read++;
                }
                l = pulse + effectL;
                r = pulse + effectR;
                wave[i * 4 + 0] = wave[i * 4 + 1] = 0; /* the speaker stays quiet */
                wave[i * 4 + 2] = l > 1 ? 1 : (l < -1 ? -1 : l);
                wave[i * 4 + 3] = r > 1 ? 1 : (r < -1 ? -1 : r);
            }
        }
        SDL_PutAudioStreamData(stream, wave, n * (int)(4 * sizeof(float)));
        frames -= n;
    }
    SDL_SetAtomicInt(&h->read, (int)read);
}

/* DOJO_HAPTICS_WAV: the effects' wave as the actuators get it, to a 48 kHz stereo WAV, for checking it without
 * a DualSense. */
static void RecordHaptics(float l, float r)
{
    static int checked;
    int16_t frame[2];
    uint32_t riff;
    if (!checked) {
        static const unsigned char header[44] = {'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E', 'f', 'm', 't',
                                                 ' ', 16, 0, 0, 0, 1, 0, 2, 0, 0x80, 0xBB, 0, 0, 0x00, 0xEE, 2,
                                                 0, 4, 0, 16, 0, 'd', 'a', 't', 'a', 0, 0, 0, 0};
        const char *path = SDL_getenv("DOJO_HAPTICS_WAV");
        checked = 1;
        sShaper.wav = path ? fopen(path, "wb") : NULL;
        if (sShaper.wav) {
            fwrite(header, 1, sizeof(header), sShaper.wav);
        }
    }
    if (!sShaper.wav) {
        return;
    }
    frame[0] = (int16_t)(l * 32767);
    frame[1] = (int16_t)(r * 32767);
    fwrite(frame, 2, 2, sShaper.wav);
    sShaper.wavBytes += 4;
    if ((sShaper.wavBytes & 0xFFFF) == 0) {
        riff = sShaper.wavBytes + 36;
        fseek(sShaper.wav, 4, SEEK_SET);
        fwrite(&riff, 4, 1, sShaper.wav);
        fseek(sShaper.wav, 40, SEEK_SET);
        fwrite(&sShaper.wavBytes, 4, 1, sShaper.wav);
        fseek(sShaper.wav, 0, SEEK_END);
        fflush(sShaper.wav);
    }
}

void InputQueueHaptics(const int16_t *stereo, int frames)
{
    /* two one-pole stages at 250 Hz; the loudness follower rises in a fraction of a millisecond, falls in 15 */
    const float lowCoefficient = 1.0f - SDL_expf(-2.0f * SDL_PI_F * 250.0f / HAPTIC_RATE);
    const float rise = 0.3f, fall = 1.0f - SDL_expf(-1.0f / (0.015f * HAPTIC_RATE));
    const float slowCoefficient = 1.0f - SDL_expf(-1.0f / (0.12f * HAPTIC_RATE));
    const double step = 44100.0 / HAPTIC_RATE;
    float strength = g_settings.rumble / 100.0f;
    int p, out = 0;
    if ((!sHaptics[0].stream && !sHaptics[1].stream && !SDL_getenv("DOJO_HAPTICS_WAV")) || frames <= 0) {
        sShaper.position = 0;
        return;
    }
    for (; sShaper.position < frames; sShaper.position += step, out++) {
        int at = (int)sShaper.position, c;
        float t = (float)(sShaper.position - at), side[2];
        for (c = 0; c < 2; c++) {
            float before = at == 0 ? sShaper.previous[c] : stereo[(at - 1) * 2 + c] / 32768.0f;
            float x = before + (stereo[at * 2 + c] / 32768.0f - before) * t, high, y;
            sShaper.low[c][0] += lowCoefficient * (x - sShaper.low[c][0]);
            sShaper.low[c][1] += lowCoefficient * (sShaper.low[c][0] - sShaper.low[c][1]);
            high = SDL_fabsf(x - sShaper.low[c][1]);
            sShaper.envelope[c] += (high > sShaper.envelope[c] ? rise : fall) * (high - sShaper.envelope[c]);
            sShaper.fast[c] += (SDL_fabsf(x) > sShaper.fast[c] ? rise : fall) * (SDL_fabsf(x) - sShaper.fast[c]);
            sShaper.slow[c] += slowCoefficient * (sShaper.fast[c] - sShaper.slow[c]);
            {
                /* full strength while the sound jumps above its recent loudness, nothing once it holds steady */
                float gate = sShaper.fast[c] > 1e-4f ? 1.5f * (sShaper.fast[c] - sShaper.slow[c]) / sShaper.fast[c] : 0;
                gate = gate < 0 ? 0 : (gate > 1 ? 1 : gate);
                y = gate * (2.5f * sShaper.low[c][1] + 2.5f * sShaper.envelope[c] * SDL_sinf(sShaper.phase));
            }
            side[c] = strength * y / (1.0f + 0.5f * SDL_fabsf(y)); /* loud ones soften, rather than clip */
        }
        sShaper.phase += 2.0f * SDL_PI_F * 180.0f / HAPTIC_RATE;
        if (sShaper.phase > 2.0f * SDL_PI_F) {
            sShaper.phase -= 2.0f * SDL_PI_F;
        }
        RecordHaptics(side[0], side[1]);
        for (p = 0; p < 2; p++) {
            Haptics *h = &sHaptics[p];
            unsigned written = (unsigned)SDL_GetAtomicInt(&h->written);
            if (h->stream && written - (unsigned)SDL_GetAtomicInt(&h->read) < HAPTIC_RING) {
                h->ring[written % HAPTIC_RING][0] = side[0];
                h->ring[written % HAPTIC_RING][1] = side[1];
                SDL_SetAtomicInt(&h->written, (int)(written + 1));
            }
        }
    }
    sShaper.position -= frames;
    sShaper.previous[0] = stereo[(frames - 1) * 2] / 32768.0f;
    sShaper.previous[1] = stereo[(frames - 1) * 2 + 1] / 32768.0f;
    (void)out;
}

static int IsWiredDualSense(int player)
{
    SDL_Gamepad *pad = sPlayerPad[player] >= 0 ? sPads[sPlayerPad[player]].pad : NULL;
    return pad && SDL_GetGamepadType(pad) == SDL_GAMEPAD_TYPE_PS5 &&
           SDL_GetGamepadConnectionState(pad) == SDL_JOYSTICK_CONNECTION_WIRED;
}

/* Opens the DualSense sound devices for the players with wired DualSenses, in order: a player's is the first
 * four-channel DualSense device the other hasn't got. */
static void OpenHaptics(void)
{
    int count = 0, i, p;
    SDL_AudioDeviceID *ids;
    sHapticsRetry = SDL_GetTicksNS() + 2000000000ull; /* its sound device can come a moment after the gamepad */
    ids = SDL_GetAudioPlaybackDevices(&count);
    for (p = 0; p < 2; p++) {
        if (sHaptics[p].stream || !IsWiredDualSense(p)) {
            continue;
        }
        for (i = 0; ids && i < count; i++) {
            const char *name = SDL_GetAudioDeviceName(ids[i]);
            SDL_AudioSpec spec;
            SDL_AudioSpec want = {SDL_AUDIO_F32, 4, HAPTIC_RATE};
            if (!name || !(SDL_strcasestr(name, "DualSense") || SDL_strcasestr(name, "Wireless Controller")) ||
                !SDL_GetAudioDeviceFormat(ids[i], &spec, NULL) || spec.channels < 4 ||
                (sHaptics[!p].stream && sHaptics[!p].device == ids[i])) {
                continue;
            }
            sHaptics[p].device = ids[i];
            SDL_SetAtomicInt(&sHaptics[p].strength, 0);
            sHaptics[p].stream = SDL_OpenAudioDeviceStream(ids[i], &want, FeedHaptics, &sHaptics[p]);
            if (sHaptics[p].stream) {
                SDL_ResumeAudioStreamDevice(sHaptics[p].stream);
                fprintf(stderr, "input: player %d's haptics on %s\n", p + 1, name);
                break;
            }
        }
    }
    SDL_free(ids);
}

int InputHapticsActive(void)
{
    return sHaptics[0].stream || sHaptics[1].stream;
}

void InputUpdateRumble(int running)
{
    int p;
    Uint64 now = SDL_GetTicksNS();
    int hidden = SDL_getenv("DOJO_HIDDEN") != NULL; /* a test run leaves the controllers alone */
    int wantHaptics = !hidden && g_settings.haptics && g_settings.rumble > 0 &&
                      (IsWiredDualSense(0) || IsWiredDualSense(1));
    if (wantHaptics && now >= sHapticsRetry &&
        ((IsWiredDualSense(0) && !sHaptics[0].stream) || (IsWiredDualSense(1) && !sHaptics[1].stream))) {
        OpenHaptics();
    } else if (!wantHaptics && InputHapticsActive()) {
        CloseHaptics(0);
        CloseHaptics(1);
    }
    for (p = 0; p < 2; p++) {
        /* Online the local fighter is one of the two, whichever controller plays it. */
        int port = NetplayActive() ? NetplayLocalPlayer() : p;
        int duty = running && !hidden ? gRumblePulses[port].dutyStep : 0;
        int strength = duty <= 0 ? 0 : (duty >= 4096 ? 10000 : duty * 10000 / 4096);
        SDL_Gamepad *pad = sPlayerPad[p] >= 0 ? sPads[sPlayerPad[p]].pad : NULL;
        strength = strength * g_settings.rumble / 100;
        if (!pad) {
            continue;
        }
        if (sHaptics[p].stream) {
            /* No motor reports to it as well: they'd put it back to emulating motors. */
            SDL_SetAtomicInt(&sHaptics[p].strength, strength);
            if (sRumbleSent[p] > 0) {
                SDL_RumbleGamepad(pad, 0, 0, 0);
            }
            sRumbleSent[p] = 0;
            continue;
        }
        /* SDL's rumble lasts as long as it's told: told again when it changes, and kept up while it runs. */
        if (strength != sRumbleSent[p] || (strength && now - sRumbleSentAt[p] > 100000000ull)) {
            Uint16 low = (Uint16)(strength * 65535 / 10000), high = (Uint16)(strength * 65535 / 10000 * 3 / 4);
            SDL_RumbleGamepad(pad, low, high, strength ? 250 : 0);
            sRumbleSent[p] = strength;
            sRumbleSentAt[p] = now;
        }
    }
}

/* -- the launcher's helpers ---------------------------------------------------------------------------------- */

static void ToolPump(Uint64 ms)
{
    Uint64 until = SDL_GetTicks() + ms;
    SDL_Event e;
    while (SDL_GetTicks() < until) {
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_EVENT_GAMEPAD_ADDED || e.type == SDL_EVENT_GAMEPAD_REMOVED) {
                InputDevicesChanged();
            }
        }
        SDL_Delay(5);
    }
}

int InputTool(const char *command, const char *arg)
{
    int i;
    if (strcmp(command, "--pads") == 0) {
        ToolPump(300); /* the gamepads' arrival */
        for (i = 0; i < sPadCount; i++) {
            SDL_Gamepad *pad = sPads[i].pad;
            printf("pad %s %s %d %s\n", sPads[i].guid, SDL_GetGamepadStringForType(SDL_GetGamepadType(pad)),
                   SDL_GetGamepadConnectionState(pad) == SDL_JOYSTICK_CONNECTION_WIRED, SDL_GetGamepadName(pad));
        }
        for (i = 0; i < 2; i++) {
            printf("player %d %d\n", i + 1, sPlayerPad[i]);
        }
        return 0;
    }
    if (strcmp(command, "--capture-pad") == 0) {
        /* The next gamepad button, trigger or stick pushed, within ten seconds. */
        Uint64 until = SDL_GetTicks() + 10000, start = SDL_GetTicks();
        SDL_Event e;
        while (SDL_GetTicks() < until) {
            while (SDL_PollEvent(&e)) {
                int binding;
                if (e.type == SDL_EVENT_GAMEPAD_ADDED || e.type == SDL_EVENT_GAMEPAD_REMOVED) {
                    InputDevicesChanged();
                }
                if (e.type == SDL_EVENT_GAMEPAD_AXIS_MOTION && SDL_GetTicks() - start < 300) {
                    continue; /* the axes' first readings */
                }
                binding = InputBindingFromEvent(&e, 1);
                if (binding) {
                    printf("bind %x\n", binding);
                    return 0;
                }
            }
            SDL_Delay(5);
        }
        printf("none\n");
        return 0;
    }
    if (strcmp(command, "--test-rumble") == 0) {
        /* Player `arg`'s rumble at full strength for a moment, as a hit would make it. */
        int player = arg && atoi(arg) == 2;
        ToolPump(300);
        for (i = 0; i < 2; i++) {
            gRumblePulses[i].dutyStep = 0;
        }
        for (i = 0; i < 60; i++) {
            gRumblePulses[player].dutyStep = i < 36 ? (short)(4096 - i * 80) : 0;
            InputUpdateRumble(1);
            ToolPump(16);
        }
        printf("%s\n", sPlayerPad[player] < 0 ? "no gamepad" : InputHapticsActive() ? "haptics" : "rumble");
        InputShutdown();
        return 0;
    }
    return -1;
}
