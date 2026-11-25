/* The options overlay (overlay.h), drawn on the CPU into a transparent picture with SDL's own renderer and
 * debug font, which each renderer lays over the game.
 *
 * Pages: the main page, the options, the controls, online play, private matches, a code to type, and a page
 * for waiting (a queue, a room, an opponent connecting). Holding Back (Backspace, or a gamepad's back button) for two
 * seconds goes to the previous page, leaves a queue, or, in an online match, leaves the match. */

#include <stdio.h>
#include <string.h>
#include "overlay.h"
#include "settings.h"
#include "online.h"
#include "input.h"

#define BACK_HOLD_MS 2000
#define TOAST_MS     5000
#define STICK_ON     20000
#define STICK_OFF    12000
#define CAPTURE_MS   6000

typedef enum Page {
    PAGE_MAIN, PAGE_OPTIONS, PAGE_CONTROLS, PAGE_ONLINE, PAGE_PRIVATE, PAGE_CODE, PAGE_WAIT, PAGE_REPLAYS, PAGE_COUNT
} Page;

enum { MAIN_RESUME, MAIN_ONLINE, MAIN_OPTIONS, MAIN_CONTROLS, MAIN_QUIT, MAIN_COUNT };
enum {
    OPT_FPS, OPT_SCALE, OPT_ASPECT, OPT_RENDERER, OPT_DISPLAY, OPT_VSYNC, OPT_VOLUME, OPT_COUNTER, OPT_DELAY,
    OPT_QUICK_PING, OPT_FIRST_TO, OPT_BACK, OPT_COUNT
};
/* The controls page: the devices and rumble, then a row of bindings for each console button. */
enum {
    CTL_PAD1, CTL_PAD2, CTL_KEYBOARD, CTL_ANALOG, CTL_DEADZONE, CTL_RUMBLE, CTL_HAPTICS, CTL_RESET, CTL_BIND,
    CTL_BACK = CTL_BIND + SETTINGS_BUTTONS, CTL_COUNT
};
#define CTL_COLUMNS (SETTINGS_BIND_SLOTS + 1) /* the bindings, then the row's reset */
enum {
    ONLINE_ROW_QUICK, ONLINE_ROW_PRIVATE, ONLINE_ROW_HILL, ONLINE_ROW_PRIVATE_HILL, ONLINE_ROW_REPLAYS, ONLINE_ROW_BACK,
    ONLINE_ROW_COUNT
};
#define REPLAYS_SHOWN 12
enum { PRIVATE_CREATE, PRIVATE_JOIN, PRIVATE_HOST_DIRECT, PRIVATE_JOIN_DIRECT, PRIVATE_BACK, PRIVATE_COUNT };
enum { CODE_ENTRY, CODE_GO, CODE_BACK, CODE_COUNT };
enum { WAIT_LEAVE, WAIT_WATCH, WAIT_COUNT };

/* The replays page's rows are the replays found, then Back. */
static int sRowCounts[PAGE_COUNT] = {MAIN_COUNT, OPT_COUNT,  CTL_COUNT,  ONLINE_ROW_COUNT,
                                     PRIVATE_COUNT, CODE_COUNT, WAIT_COUNT, 1};
static const char *const sTitles[PAGE_COUNT] = {"DOJO",          "OPTIONS",      "CONTROLS",    "PLAY ONLINE",
                                                "PRIVATE MATCH", "ENTER A CODE", "PLAY ONLINE", "REPLAYS"};
static char sReplays[REPLAYS_SHOWN][96];
static int sReplayCount;
static const char *const sMainLabels[MAIN_COUNT] = {"Resume", "Play online", "Options", "Controls", "Quit game"};
static const char *const sOptionLabels[OPT_COUNT] = {
    "Frame rate", "Resolution", "Aspect ratio", "Renderer", "Display", "VSync", "Volume", "Frame counter",
    "Online input delay", "Quick play ping", "Online set", "Back",
};
static const char *const sControlLabels[CTL_BIND] = {
    "Player 1 gamepad", "Player 2 gamepad", "Keyboard & mouse", "Left stick moves", "Stick dead zone", "Rumble",
    "DualSense haptics", "Reset all bindings",
};
static const char *const sOnlineLabels[ONLINE_ROW_COUNT] = {"Quick play", "Private match", "King of the Hill",
                                                            "Private hill (by code)", "Replays", "Back"};
static const char *const sPrivateLabels[PRIVATE_COUNT] = {"Create a room", "Join a room",
                                                          "Host directly (no server)", "Join directly by address",
                                                          "Back"};

static OverlayContext sContext;
static void (*sApply)(OverlayChange change);
static int sOpen, sDirty = 1, sShown;
static Page sPage;
static int sRow;
enum { CODE_ROOM, CODE_HILL, CODE_ADDRESS };
static int sCodeKind;        /* what the code page asks for: a room's code, a hill's, or an address */
static char sCode[48];
static int sCodeLetter;      /* the character a gamepad is choosing for the next place */
#define ADDRESS_CHARS "0123456789.:"
static SDL_Surface *sCanvas;
static SDL_Renderer *sDraw;
static Uint64 sCountStart;
static int sCount, sFpsShown = -1;
static Uint64 sBackSince;    /* when Back went down, or 0 */
static int sBackDone;        /* the hold did its work: nothing more until Back is let go */
static int sBackShown;       /* the hold's progress in percent, as drawn */
static char sToast[160];
static Uint64 sToastUntil;
static char sBanner[160];
static OnlineState sLastOnline = ONLINE_OFF;
static int sStickY, sStickX; /* -1, 0, 1: the stick's direction, for a step only on a new push */
static int sCol;             /* the controls page's column on a binding row */
static int sScroll;          /* the first line shown of a page too long for the panel */
static Uint64 sCaptureSince; /* when a binding began waiting for its button, or 0 */

void OverlayInit(const OverlayContext *context, void (*apply)(OverlayChange change))
{
    sContext = *context;
    sApply = apply;
    sCanvas = SDL_CreateSurface(OVERLAY_WIDTH, OVERLAY_HEIGHT, SDL_PIXELFORMAT_ABGR8888);
    sDraw = sCanvas ? SDL_CreateSoftwareRenderer(sCanvas) : NULL;
    sCountStart = SDL_GetTicksNS();
}

void OverlayUpdateContext(const OverlayContext *context)
{
    if (memcmp(context, &sContext, sizeof(sContext)) != 0) {
        sContext = *context;
        sDirty = 1;
    }
}

int OverlayIsOpen(void)
{
    return sOpen;
}

static void Go(Page page)
{
    sPage = page;
    sRow = 0;
    sScroll = 0;
    sCaptureSince = 0;
    sDirty = 1;
}

static void SetOpen(int open)
{
    if (sOpen == open) {
        return;
    }
    sOpen = open;
    sDirty = 1;
    if (open) {
        OnlineState s = OnlineGetState();
        Go(s == ONLINE_WAITING || s == ONLINE_SETUP ? PAGE_WAIT : PAGE_MAIN);
    } else {
        SettingsSave();
    }
}

void OverlayOpen(void)
{
    SetOpen(1);
}

static void Toast(const char *text)
{
    snprintf(sToast, sizeof(sToast), "%s", text);
    sToastUntil = SDL_GetTicksNS() + (Uint64)TOAST_MS * 1000000ull;
    sDirty = 1;
}

/* The previous page; from the waiting page, leaving the queue or room. */
static void Back(void)
{
    switch (sPage) {
    case PAGE_MAIN:
        SetOpen(0);
        break;
    case PAGE_OPTIONS:
        Go(PAGE_MAIN);
        sRow = MAIN_OPTIONS;
        break;
    case PAGE_CONTROLS:
        Go(PAGE_MAIN);
        sRow = MAIN_CONTROLS;
        break;
    case PAGE_ONLINE:
        Go(PAGE_MAIN);
        sRow = MAIN_ONLINE;
        break;
    case PAGE_PRIVATE:
        Go(PAGE_ONLINE);
        sRow = ONLINE_ROW_PRIVATE;
        break;
    case PAGE_CODE:
        Go(sCodeKind == CODE_HILL ? PAGE_ONLINE : PAGE_PRIVATE);
        break;
    case PAGE_WAIT:
        OnlineLeave();
        Go(PAGE_ONLINE);
        break;
    case PAGE_REPLAYS:
        Go(PAGE_ONLINE);
        sRow = ONLINE_ROW_REPLAYS;
        break;
    default:
        break;
    }
}

static const int sFpsChoices[] = {0, 30, 60, 90, 120, 150, 180, 240, 300, 360, 480};
#define FPS_CHOICES ((int)(sizeof(sFpsChoices) / sizeof(sFpsChoices[0])))

static int Step(int value, int dir, int lo, int hi)
{
    value += dir;
    return value < lo ? hi : (value > hi ? lo : value);
}

static void ChangeOption(int dir)
{
    Settings *s = &g_settings;
    int i;
    switch (sRow) {
    case OPT_FPS:
        if (sContext.online) {
            return;
        }
        for (i = 0; i < FPS_CHOICES && sFpsChoices[i] != s->fps; i++) {
        }
        s->fps = sFpsChoices[Step(i == FPS_CHOICES ? 0 : i, dir ? dir : 1, 0, FPS_CHOICES - 1)];
        sApply(OVERLAY_CHANGE_FPS);
        break;
    case OPT_SCALE:
        if (!sContext.vulkan) {
            return;
        }
        s->scale = Step(s->scale, dir ? dir : 1, 1, 8);
        sApply(OVERLAY_CHANGE_SCALE);
        break;
    case OPT_ASPECT:
        s->aspect = Step(s->aspect, dir ? dir : 1, 0, SETTINGS_ASPECT_COUNT - 1);
        break;
    case OPT_RENDERER:
        s->software = !s->software;
        break;
    case OPT_DISPLAY:
        s->fullscreen = !s->fullscreen;
        sApply(OVERLAY_CHANGE_FULLSCREEN);
        break;
    case OPT_VSYNC:
        s->vsync = !s->vsync;
        sApply(OVERLAY_CHANGE_VSYNC);
        break;
    case OPT_VOLUME:
        s->volume += (dir ? dir : 1) * 10;
        s->volume = s->volume < 0 ? 0 : (s->volume > 100 ? 100 : s->volume);
        sApply(OVERLAY_CHANGE_VOLUME);
        break;
    case OPT_COUNTER:
        s->showFps = !s->showFps;
        break;
    case OPT_DELAY:
        s->onlineDelay = Step(s->onlineDelay, dir ? dir : 1, -1, 8); /* the next match on */
        break;
    case OPT_FIRST_TO: {
        static const int choices[] = {0, 1, 2, 3, 5};
        int at = 0;
        while (at < 5 && choices[at] != s->firstTo) {
            at++;
        }
        s->firstTo = choices[Step(at == 5 ? 0 : at, dir ? dir : 1, 0, 4)];
        break;
    }
    case OPT_QUICK_PING: {
        static const int choices[] = {0, 80, 120, 160, 200, 250, 300};
        int n = (int)(sizeof(choices) / sizeof(choices[0])), at = 0;
        while (at < n && choices[at] != s->quickMaxPing) {
            at++;
        }
        s->quickMaxPing = choices[Step(at == n ? 3 : at, dir ? dir : 1, 0, n - 1)];
        break;
    }
    case OPT_BACK:
        if (dir == 0) {
            Back();
        }
        return;
    }
    sDirty = 1;
}

/* Left or right (dir -1 or 1), or confirm (dir 0), on the controls page: on a binding row the arrows move
 * between the columns and confirm waits for the button to bind, or resets the row. */
static void ChangeControl(int dir)
{
    Settings *s = &g_settings;
    if (sRow >= CTL_BIND && sRow < CTL_BACK) {
        if (dir != 0) {
            sCol = sCol + dir < 0 ? 0 : (sCol + dir >= CTL_COLUMNS ? CTL_COLUMNS - 1 : sCol + dir);
        } else if (sCol == SETTINGS_BIND_SLOTS) {
            InputDefaultBindings(sRow - CTL_BIND);
        } else {
            sCaptureSince = SDL_GetTicksNS();
        }
        sDirty = 1;
        return;
    }
    switch (sRow) {
    case CTL_PAD1:
    case CTL_PAD2:
        InputStepPlayerPad(sRow - CTL_PAD1, dir);
        break;
    case CTL_KEYBOARD:
        s->keyboardPlayer = !s->keyboardPlayer;
        break;
    case CTL_ANALOG:
        s->analog = !s->analog;
        break;
    case CTL_DEADZONE:
        s->deadzone = Step(s->deadzone / 10, dir ? dir : 1, 1, 9) * 10;
        break;
    case CTL_RUMBLE:
        s->rumble = Step(s->rumble / 10, dir ? dir : 1, 0, 10) * 10;
        break;
    case CTL_HAPTICS:
        s->haptics = !s->haptics;
        break;
    case CTL_RESET:
        if (dir == 0) {
            InputDefaultBindings(-1);
            Toast("Every binding is back to its default.");
        }
        break;
    case CTL_BACK:
        if (dir == 0) {
            Back();
        }
        return;
    }
    sDirty = 1;
}

/* The binding being waited for takes an event: a gamepad's for a gamepad column, else a key or a click.
 * Escape gives up; Delete clears it. */
static int Capture(const SDL_Event *e)
{
    int *slot = &g_settings.binds[sRow - CTL_BIND][sCol];
    int pad = sCol < INPUT_PAD_SLOTS, binding;
    if (e->type == SDL_EVENT_KEY_DOWN && !e->key.repeat &&
        (e->key.scancode == SDL_SCANCODE_ESCAPE || e->key.scancode == SDL_SCANCODE_F1 ||
         e->key.scancode == SDL_SCANCODE_DELETE)) {
        if (e->key.scancode == SDL_SCANCODE_DELETE) {
            *slot = 0;
        }
        sCaptureSince = 0;
        sDirty = 1;
        return 1;
    }
    if (e->type == SDL_EVENT_GAMEPAD_AXIS_MOTION && SDL_GetTicksNS() - sCaptureSince < 250000000ull) {
        return 1; /* the stick that chose the cell, still on its way back */
    }
    binding = InputBindingFromEvent(e, pad);
    if (binding) {
        *slot = binding;
        sCaptureSince = 0;
        sDirty = 1;
        return 1;
    }
    /* everything else waits, the other kind of device's buttons too */
    return e->type == SDL_EVENT_KEY_DOWN || e->type == SDL_EVENT_KEY_UP || e->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN ||
           e->type == SDL_EVENT_GAMEPAD_BUTTON_UP || e->type == SDL_EVENT_GAMEPAD_AXIS_MOTION ||
           e->type == SDL_EVENT_MOUSE_BUTTON_DOWN || e->type == SDL_EVENT_MOUSE_BUTTON_UP;
}

static void ClearBinding(void)
{
    if (sPage == PAGE_CONTROLS && sRow >= CTL_BIND && sRow < CTL_BACK && sCol < SETTINGS_BIND_SLOTS) {
        g_settings.binds[sRow - CTL_BIND][sCol] = 0;
        sDirty = 1;
    }
}

static int CodeLength(void)
{
    return sCodeKind == CODE_ADDRESS ? (int)sizeof(sCode) - 1 : 6;
}

/* The character a gamepad's up and down are on. */
static char ChosenChar(void)
{
    return sCodeKind == CODE_ADDRESS ? ADDRESS_CHARS[sCodeLetter % (int)strlen(ADDRESS_CHARS)]
                                     : (char)('A' + sCodeLetter % 26);
}

static void CodeLetter(char c)
{
    size_t n = strlen(sCode);
    if ((int)n < CodeLength()) {
        sCode[n] = c;
        sCode[n + 1] = 0;
        sDirty = 1;
    }
}

static void CodeDelete(void)
{
    size_t n = strlen(sCode);
    if (n > 0) {
        sCode[n - 1] = 0;
        sDirty = 1;
    }
}

static void CodeSubmit(void)
{
    if (sCodeKind == CODE_ADDRESS) {
        if (!sCode[0]) {
            Toast("Type the host's address, as 203.0.113.5 or 203.0.113.5:7778.");
            return;
        }
        OnlineJoinDirect(sCode);
        Go(PAGE_WAIT);
        return;
    }
    if (strlen(sCode) != 6) {
        Toast("A code has six letters.");
        return;
    }
    if (sCodeKind == CODE_HILL) {
        OnlineKingOfTheHill(sCode);
    } else {
        OnlineJoinRoom(sCode);
    }
    Go(PAGE_WAIT);
}

static void AskCode(int kind)
{
    sCodeKind = kind;
    sCode[0] = 0;
    sCodeLetter = 0;
    Go(PAGE_CODE);
}

/* Left or right (dir -1 or 1) on the current row, or confirm (dir 0). */
static void Change(int dir)
{
    switch (sPage) {
    case PAGE_MAIN:
        if (dir != 0) {
            return;
        }
        switch (sRow) {
        case MAIN_RESUME: SetOpen(0); break;
        case MAIN_ONLINE:
            if (!g_settings.offline) {
                OnlineConnect();
            }
            Go(PAGE_ONLINE);
            break;
        case MAIN_OPTIONS: Go(PAGE_OPTIONS); break;
        case MAIN_CONTROLS: Go(PAGE_CONTROLS); sCol = 0; break;
        case MAIN_QUIT: SettingsSave(); sApply(OVERLAY_CHANGE_QUIT); break;
        }
        break;
    case PAGE_OPTIONS:
        ChangeOption(dir);
        break;
    case PAGE_CONTROLS:
        ChangeControl(dir);
        break;
    case PAGE_ONLINE:
        if (dir != 0) {
            return;
        }
        if (sRow == ONLINE_ROW_BACK) {
            Back();
            return;
        }
        if (sRow == ONLINE_ROW_PRIVATE) {
            Go(PAGE_PRIVATE); /* player to player needs no server */
            return;
        }
        if (sRow == ONLINE_ROW_REPLAYS) {
            sReplayCount = OnlineListReplays(sReplays, REPLAYS_SHOWN); /* no server needed either */
            sRowCounts[PAGE_REPLAYS] = sReplayCount + 1;
            Go(PAGE_REPLAYS);
            return;
        }
        if (g_settings.offline) {
            Toast("Offline mode: only Private Match's direct matches work.");
            return;
        }
        if (OnlineGetState() == ONLINE_OFF) {
            OnlineConnect();
            Toast("Connecting to the server...");
            return;
        }
        if (OnlineGetState() != ONLINE_IDLE) {
            Toast("Still connecting: a moment.");
            return;
        }
        switch (sRow) {
        case ONLINE_ROW_QUICK: OnlineQuickPlay(); Go(PAGE_WAIT); break;
        case ONLINE_ROW_PRIVATE: Go(PAGE_PRIVATE); break;
        case ONLINE_ROW_HILL: OnlineKingOfTheHill(""); Go(PAGE_WAIT); break;
        case ONLINE_ROW_PRIVATE_HILL:
            AskCode(CODE_HILL);
            break;
        }
        break;
    case PAGE_PRIVATE:
        if (dir != 0) {
            return;
        }
        if ((sRow == PRIVATE_CREATE || sRow == PRIVATE_JOIN) && g_settings.offline) {
            Toast("Offline mode: rooms need the server. Host or join directly instead.");
            return;
        }
        if ((sRow == PRIVATE_CREATE || sRow == PRIVATE_JOIN) && OnlineGetState() != ONLINE_IDLE) {
            OnlineConnect();
            Toast(OnlineGetState() == ONLINE_OFF ? "That needs the server, which can't be reached."
                                                 : "Connecting to the server: a moment.");
            return;
        }
        switch (sRow) {
        case PRIVATE_CREATE: OnlineCreateRoom(); Go(PAGE_WAIT); break;
        case PRIVATE_JOIN: AskCode(CODE_ROOM); break;
        case PRIVATE_HOST_DIRECT: OnlineHostDirect(); Go(PAGE_WAIT); break;
        case PRIVATE_JOIN_DIRECT: AskCode(CODE_ADDRESS); break;
        case PRIVATE_BACK: Back(); break;
        }
        break;
    case PAGE_CODE:
        if (sRow == CODE_ENTRY) {
            if (dir < 0) {
                CodeDelete();
            } else if (dir > 0) {
                CodeLetter(ChosenChar());
            } else {
                CodeSubmit();
            }
        } else if (dir == 0) {
            if (sRow == CODE_GO) {
                CodeSubmit();
            } else {
                Back();
            }
        }
        break;
    case PAGE_WAIT:
        if (sRow == WAIT_WATCH) {
            if (g_settings.watch) {
                OnlineStopWatching();
            } else {
                g_settings.watch = 1;
            }
            sDirty = 1;
        } else if (dir == 0) {
            Back();
        }
        break;
    case PAGE_REPLAYS:
        if (dir != 0) {
            return;
        }
        if (sRow >= sReplayCount) {
            Back();
        } else if (OnlinePlayReplay(sReplays[sRow])) {
            SetOpen(0);
        }
        break;
    default:
        break;
    }
}

static void Move(int dir)
{
    if (sPage == PAGE_CODE && sRow == CODE_ENTRY && dir != 0 && (int)strlen(sCode) < CodeLength()) {
        /* up and down choose the character a gamepad adds with right; down from the last leaves the row */
        int last = sCodeKind == CODE_ADDRESS ? (int)strlen(ADDRESS_CHARS) - 1 : 25;
        if (dir > 0 && sCodeLetter == last) {
            sRow = CODE_GO;
        } else {
            sCodeLetter = Step(sCodeLetter, dir, 0, last);
        }
        sDirty = 1;
        return;
    }
    sRow = Step(sRow, dir, 0, sRowCounts[sPage] - 1);
    sDirty = 1;
}

static void BackDown(void)
{
    if (!sBackSince) {
        sBackSince = SDL_GetTicksNS();
        sBackDone = 0;
    }
}

static void BackUp(void)
{
    sBackSince = 0;
    sBackDone = 0;
    if (sBackShown) {
        sBackShown = 0;
        sDirty = 1;
    }
}

int OverlayHandleEvent(const SDL_Event *e)
{
    if (sOpen && sCaptureSince && Capture(e)) {
        return 1;
    }
    if (e->type == SDL_EVENT_KEY_DOWN || e->type == SDL_EVENT_KEY_UP) {
        SDL_Scancode key = e->key.scancode;
        int down = e->type == SDL_EVENT_KEY_DOWN;
        /* Back is held to go back; with the menu shut the game still has the key (its select button). */
        if (key == SDL_SCANCODE_BACKSPACE) {
            if (down && !e->key.repeat) {
                BackDown();
                if (sOpen && sPage == PAGE_CODE && sRow == CODE_ENTRY) {
                    CodeDelete();
                }
            } else if (!down) {
                BackUp();
            }
            return sOpen;
        }
        if (!down) {
            return sOpen;
        }
        if (key == SDL_SCANCODE_F1 || key == SDL_SCANCODE_ESCAPE) {
            if (!e->key.repeat) {
                SetOpen(!sOpen);
            }
            return 1;
        }
        if (!sOpen) {
            return 0;
        }
        if (sPage == PAGE_CODE && sRow == CODE_ENTRY) {
            char c = 0;
            if (key >= SDL_SCANCODE_A && key <= SDL_SCANCODE_Z) {
                c = (char)((sCodeKind == CODE_ADDRESS ? 'a' : 'A') + (key - SDL_SCANCODE_A));
            } else if (sCodeKind == CODE_ADDRESS) {
                if (key >= SDL_SCANCODE_1 && key <= SDL_SCANCODE_9) {
                    c = (char)('1' + (key - SDL_SCANCODE_1));
                } else if (key == SDL_SCANCODE_0) {
                    c = '0';
                } else if (key >= SDL_SCANCODE_KP_1 && key <= SDL_SCANCODE_KP_9) {
                    c = (char)('1' + (key - SDL_SCANCODE_KP_1));
                } else if (key == SDL_SCANCODE_KP_0) {
                    c = '0';
                } else if (key == SDL_SCANCODE_PERIOD || key == SDL_SCANCODE_KP_PERIOD) {
                    c = '.';
                } else if (key == SDL_SCANCODE_SEMICOLON) {
                    c = ':';
                } else if (key == SDL_SCANCODE_MINUS) {
                    c = '-';
                }
            }
            if (c) {
                CodeLetter(c);
                return 1;
            }
        }
        switch (key) {
        case SDL_SCANCODE_UP: Move(-1); break;
        case SDL_SCANCODE_DOWN: Move(1); break;
        case SDL_SCANCODE_LEFT: Change(-1); break;
        case SDL_SCANCODE_RIGHT: Change(1); break;
        case SDL_SCANCODE_RETURN:
        case SDL_SCANCODE_KP_ENTER:
        case SDL_SCANCODE_SPACE:
        case SDL_SCANCODE_X: Change(0); break;
        case SDL_SCANCODE_C:
            if (sPage != PAGE_WAIT) {
                Back();
            }
            break;
        case SDL_SCANCODE_DELETE: ClearBinding(); break;
        default: break;
        }
        return 1;
    }
    if (e->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN || e->type == SDL_EVENT_GAMEPAD_BUTTON_UP) {
        int down = e->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN;
        if (e->gbutton.button == SDL_GAMEPAD_BUTTON_BACK) {
            if (down) {
                BackDown();
            } else {
                BackUp();
            }
            return sOpen;
        }
        if (!down) {
            return sOpen;
        }
        if (e->gbutton.button == SDL_GAMEPAD_BUTTON_GUIDE) {
            SetOpen(!sOpen);
            return 1;
        }
        if (!sOpen) {
            return 0;
        }
        switch (e->gbutton.button) {
        case SDL_GAMEPAD_BUTTON_DPAD_UP: Move(-1); break;
        case SDL_GAMEPAD_BUTTON_DPAD_DOWN: Move(1); break;
        case SDL_GAMEPAD_BUTTON_DPAD_LEFT: Change(-1); break;
        case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: Change(1); break;
        case SDL_GAMEPAD_BUTTON_SOUTH: Change(0); break;
        case SDL_GAMEPAD_BUTTON_EAST:
            if (sPage != PAGE_WAIT) {
                Back();
            }
            break;
        case SDL_GAMEPAD_BUTTON_NORTH: ClearBinding(); break;
        case SDL_GAMEPAD_BUTTON_START: SetOpen(0); break;
        default: break;
        }
        return 1;
    }
    if (e->type == SDL_EVENT_GAMEPAD_AXIS_MOTION && sOpen) {
        /* The left stick works the menus too: a push past the middle is one step. */
        int v = e->gaxis.value;
        if (e->gaxis.axis == SDL_GAMEPAD_AXIS_LEFTY) {
            int dir = v < -STICK_ON ? -1 : (v > STICK_ON ? 1 : (v > -STICK_OFF && v < STICK_OFF ? 0 : sStickY));
            if (dir != sStickY && dir != 0) {
                Move(dir);
            }
            sStickY = dir;
            return 1;
        }
        if (e->gaxis.axis == SDL_GAMEPAD_AXIS_LEFTX) {
            int dir = v < -STICK_ON ? -1 : (v > STICK_ON ? 1 : (v > -STICK_OFF && v < STICK_OFF ? 0 : sStickX));
            if (dir != sStickX && dir != 0) {
                Change(dir);
            }
            sStickX = dir;
            return 1;
        }
    }
    return 0;
}

void OverlayFramePresented(void)
{
    Uint64 now = SDL_GetTicksNS();
    sCount++;
    if (now - sCountStart >= 1000000000ull) {
        int fps = (int)((Uint64)sCount * 1000000000ull / (now - sCountStart));
        sCount = 0;
        sCountStart = now;
        if (fps != sFpsShown) {
            sFpsShown = fps;
            if (g_settings.showFps) {
                sDirty = 1;
            }
        }
    }
}

static void OptionValue(int row, char *out, size_t size)
{
    const Settings *s = &g_settings;
    switch (row) {
    case OPT_FPS:
        if (sContext.online) {
            snprintf(out, size, "%d (set by the online match)", sContext.ticksPerFrame * 30);
        } else if (s->fps == 0) {
            snprintf(out, size, "Auto (%d Hz display: %d)", sContext.displayHz, sContext.ticksPerFrame * 30);
        } else {
            snprintf(out, size, "%d", s->fps);
        }
        break;
    case OPT_SCALE:
        if (!sContext.vulkan) {
            snprintf(out, size, "1x (software renderer)");
        } else {
            snprintf(out, size, "%dx (%d lines)", s->scale, s->scale * 240);
        }
        break;
    case OPT_ASPECT:
        if (s->aspect == SETTINGS_ASPECT_AUTO && sContext.displayW > 0) {
            snprintf(out, size, "Auto (%dx%d display)%s", sContext.displayW, sContext.displayH,
                     s->aspect != sContext.runningAspect ? "  (on restart)" : "");
        } else {
            snprintf(out, size, "%s%s",
                     s->aspect == SETTINGS_ASPECT_AUTO ? "Auto (the display's)" : SETTINGS_ASPECTS[s->aspect],
                     s->aspect != sContext.runningAspect ? "  (on restart)" : "");
        }
        break;
    case OPT_RENDERER:
        snprintf(out, size, "%s%s", s->software ? "Software" : "Vulkan",
                 s->software != sContext.runningSoftware ? "  (on restart)" : "");
        break;
    case OPT_DISPLAY:
        snprintf(out, size, "%s", s->fullscreen ? "Full screen" : "Window");
        break;
    case OPT_VSYNC:
        snprintf(out, size, "%s", s->vsync ? "On" : "Off");
        break;
    case OPT_VOLUME:
        snprintf(out, size, "%d%%", s->volume);
        break;
    case OPT_COUNTER:
        snprintf(out, size, "%s", s->showFps ? "On" : "Off");
        break;
    case OPT_FIRST_TO:
        if (s->firstTo) {
            snprintf(out, size, "First to %d (the longer of the two players' wins)", s->firstTo);
        } else {
            snprintf(out, size, "No end (play until someone leaves)");
        }
        break;
    case OPT_QUICK_PING:
        if (s->quickMaxPing) {
            snprintf(out, size, "Up to %d ms (anyone after 30 s)", s->quickMaxPing);
        } else {
            snprintf(out, size, "Anyone");
        }
        break;
    case OPT_DELAY:
        if (s->onlineDelay < 0) {
            snprintf(out, size, "Auto (the least the connection allows)");
        } else {
            snprintf(out, size, "%d field%s (%d ms)", s->onlineDelay, s->onlineDelay == 1 ? "" : "s",
                     (s->onlineDelay * 1001 + 30) / 60);
        }
        break;
    default:
        out[0] = 0;
        break;
    }
}

static void ControlValue(int row, char *out, size_t size)
{
    const Settings *s = &g_settings;
    switch (row) {
    case CTL_PAD1:
    case CTL_PAD2:
        InputDescribePlayerPad(row - CTL_PAD1, out, size);
        break;
    case CTL_KEYBOARD:
        snprintf(out, size, "Player %d", s->keyboardPlayer + 1);
        break;
    case CTL_ANALOG:
        snprintf(out, size, "%s", s->analog ? "On (as the d-pad)" : "Off");
        break;
    case CTL_DEADZONE:
        snprintf(out, size, "%d%%", s->deadzone);
        break;
    case CTL_RUMBLE:
        snprintf(out, size, s->rumble ? "%d%%" : "Off", s->rumble);
        break;
    case CTL_HAPTICS:
        snprintf(out, size, "%s", !s->haptics      ? "Off (motor rumble)"
                                  : InputHapticsActive() ? "On (in use)"
                                                         : "On (a DualSense on a USB cable)");
        break;
    default:
        out[0] = 0;
        break;
    }
}

static void Fill(float x, float y, float w, float h, Uint8 r, Uint8 g, Uint8 b, Uint8 a)
{
    SDL_FRect rect = {x, y, w, h};
    SDL_SetRenderDrawColor(sDraw, r, g, b, a);
    SDL_RenderFillRect(sDraw, &rect);
}

static void Text(float x, float y, float scale, const char *text, Uint8 r, Uint8 g, Uint8 b)
{
    SDL_SetRenderScale(sDraw, scale, scale);
    SDL_SetRenderDrawColor(sDraw, r, g, b, 255);
    SDL_RenderDebugText(sDraw, x / scale, y / scale, text);
    SDL_SetRenderScale(sDraw, 1, 1);
}

/* Text wrapped to `width` pixels; returns the lines used. */
static int Wrapped(float x, float y, float width, const char *text, Uint8 r, Uint8 g, Uint8 b)
{
    int perLine = (int)(width / 8), lines = 0;
    while (*text) {
        char line[96];
        int n = (int)strlen(text), cut = n;
        if (n > perLine) {
            cut = perLine;
            while (cut > 0 && text[cut] != ' ') {
                cut--;
            }
            if (cut == 0) {
                cut = perLine;
            }
        }
        if (cut > (int)sizeof(line) - 1) {
            cut = (int)sizeof(line) - 1;
        }
        memcpy(line, text, (size_t)cut);
        line[cut] = 0;
        Text(x, y + (float)lines * 12, 1, line, r, g, b);
        lines++;
        text += cut;
        while (*text == ' ') {
            text++;
        }
    }
    return lines;
}

static const char *RowLabel(int row, char *buffer, size_t size)
{
    switch (sPage) {
    case PAGE_MAIN: return sMainLabels[row];
    case PAGE_OPTIONS: return sOptionLabels[row];
    case PAGE_CONTROLS:
        return row < CTL_BIND ? sControlLabels[row] : row < CTL_BACK ? INPUT_BUTTONS[row - CTL_BIND].name : "Back";
    case PAGE_ONLINE: return sOnlineLabels[row];
    case PAGE_REPLAYS:
        if (row < sReplayCount) {
            /* "2026-11-25_141502_brett_vs_kaz.dojoreplay" as "2026-11-25 14:15  brett vs kaz" */
            const char *name = sReplays[row], *vs = strstr(name, "_vs_"), *dot = strstr(name, ".dojoreplay");
            const char *who = strlen(name) > 18 ? name + 18 : name;
            if (strlen(name) > 18 && vs && dot && vs > who) {
                snprintf(buffer, size, "%.10s %.2s:%.2s  %.*s vs %.*s", name, name + 11, name + 13, (int)(vs - who),
                         who, (int)(dot - vs - 4), vs + 4);
            } else {
                snprintf(buffer, size, "%s", name);
            }
            return buffer;
        }
        return sReplayCount ? "Back" : "No replays yet (online matches are saved)   Back";
    case PAGE_PRIVATE: return sPrivateLabels[row];
    case PAGE_CODE:
        if (row == CODE_ENTRY) {
            if (sCodeKind == CODE_ADDRESS) {
                snprintf(buffer, size, "Address:  %s%c", sCode, (int)strlen(sCode) < CodeLength() ? '_' : ' ');
            } else {
                char shown[7];
                int i, n = (int)strlen(sCode);
                for (i = 0; i < 6; i++) {
                    shown[i] = i < n ? sCode[i] : (i == n ? (char)('a' + sCodeLetter) : '_');
                }
                shown[6] = 0;
                snprintf(buffer, size, "Code:  %c %c %c %c %c %c", shown[0], shown[1], shown[2], shown[3], shown[4],
                         shown[5]);
            }
            return buffer;
        }
        return row == CODE_GO ? (sCodeKind == CODE_HILL ? "Join the hill" : sCodeKind == CODE_ROOM ? "Join the room"
                                                                                                 : "Connect")
                              : "Back";
    case PAGE_WAIT:
        if (row == WAIT_WATCH) {
            snprintf(buffer, size, "Watch the hill's matches: %s", g_settings.watch ? "On" : "Off");
            return buffer;
        }
        return "Leave";
    default: return "";
    }
}

/* The controls table's columns: where each starts, and its width. */
#define CELL_X0 150.0f
#define CELL_W  98.0f
#define CELL_GAP 4.0f
#define RESET_W 48.0f
#define ROW_H   20.0f
#define TABLE_HEADER_H 30.0f

static void TextCentred(float x, float y, float w, const char *text, Uint8 r, Uint8 g, Uint8 b)
{
    char shown[16];
    int n = (int)(w / 8) - 1, len = (int)strlen(text);
    if (len > n) {
        snprintf(shown, sizeof(shown), "%.*s.", n - 1, text); /* too long: cut, with a dot */
        text = shown;
        len = n;
    }
    Text(x + (w - 8 * (float)len) / 2, y, 1, text, r, g, b);
}

static void DrawTableHeader(float left, float y)
{
    float x = left + CELL_X0;
    int i;
    Text(left + 22, y + 14, 1, "Button", 150, 140, 125);
    TextCentred(x, y, 2 * CELL_W + CELL_GAP, "Gamepad", 200, 190, 150);
    TextCentred(x + 2 * (CELL_W + CELL_GAP), y, 2 * CELL_W + CELL_GAP, "Keyboard & mouse", 200, 190, 150);
    for (i = 0; i < SETTINGS_BIND_SLOTS; i++) {
        char name[16];
        snprintf(name, sizeof(name), "Input %d", i % INPUT_PAD_SLOTS + 1);
        TextCentred(x + (float)i * (CELL_W + CELL_GAP), y + 14, CELL_W, name, 150, 140, 125);
    }
}

static void DrawBindingRow(float left, float y, int row, int selected)
{
    int slot;
    float x = left + CELL_X0;
    for (slot = 0; slot <= SETTINGS_BIND_SLOTS; slot++) {
        float w = slot == SETTINGS_BIND_SLOTS ? RESET_W : CELL_W;
        int on = selected && slot == sCol;
        char name[40];
        if (on) {
            Fill(x - 1, y - 6, w + 2, ROW_H, 235, 220, 190, 255);
            Fill(x, y - 5, w, ROW_H - 2, 200, 60, 40, 255);
        } else {
            Fill(x, y - 5, w, ROW_H - 2, 40, 30, 26, 255);
        }
        if (slot == SETTINGS_BIND_SLOTS) {
            snprintf(name, sizeof(name), "Reset");
        } else if (on && sCaptureSince) {
            snprintf(name, sizeof(name), "%s", slot < INPUT_PAD_SLOTS ? "Press..." : "Key/click...");
        } else {
            InputBindingName(g_settings.binds[row - CTL_BIND][slot], name, sizeof(name));
        }
        {
            int none = slot < SETTINGS_BIND_SLOTS && !g_settings.binds[row - CTL_BIND][slot] && !on;
            TextCentred(x, y, w, name, on ? 255 : (none ? 120 : 225), on ? 245 : (none ? 112 : 215),
                        on ? 230 : (none ? 100 : 195));
        }
        x += w + CELL_GAP;
    }
}

/* The height of a page's row, with the controls table's header before its first binding. */
static float RowHeight(int row)
{
    return sPage == PAGE_CONTROLS && row == CTL_BIND ? ROW_H + TABLE_HEADER_H : ROW_H;
}

static void DrawMenu(void)
{
    const int wide = sPage == PAGE_CONTROLS;
    const float panelW = wide ? 620 : 470, panelH = wide ? 350 : 320;
    const float left = (OVERLAY_WIDTH - panelW) / 2, top = (OVERLAY_HEIGHT - panelH) / 2;
    float y, bottom = top + panelH - 30;
    int row;
    const char *hint;
    Fill(0, 0, OVERLAY_WIDTH, OVERLAY_HEIGHT, 0, 0, 0, 110);
    Fill(left, top, panelW, panelH, 16, 12, 10, 235);
    Fill(left, top, panelW, 2, 190, 40, 30, 255);
    Fill(left, top + panelH - 2, panelW, 2, 190, 40, 30, 255);
    Text(left + 20, top + 14, 2, sTitles[sPage], 235, 220, 190);
    y = top + 44;
    if (sPage == PAGE_ONLINE || sPage == PAGE_WAIT || sPage == PAGE_PRIVATE || sPage == PAGE_CODE) {
        char status[200];
        OnlineDescribe(status, sizeof(status));
        y += 12 * (float)Wrapped(left + 22, y, panelW - 44, status, 200, 190, 150);
        if (sPage == PAGE_ONLINE) {
            char who[64];
            snprintf(who, sizeof(who), "Playing as %s", g_settings.name);
            Text(left + 22, y, 1, who, 140, 130, 115);
            y += 12;
        }
        if (sPage == PAGE_CODE) {
            y += 12 * (float)Wrapped(left + 22, y, panelW - 44,
                                     sCodeKind == CODE_HILL ? "The hill's code: six letters. A new code makes a new hill."
                                     : sCodeKind == CODE_ROOM ? "The code your friend's game shows: six letters."
                                     : "The host's address (their game shows it), as 192.168.1.20 or 203.0.113.5:7778.",
                                     140, 130, 115);
        }
        y += 8;
    }
    {
        /* A page longer than the panel scrolls to keep the chosen row in sight. */
        float space = bottom - y, used = 0;
        if (sRow < sScroll) {
            sScroll = sRow;
        }
        for (row = sScroll; row <= sRow; row++) {
            used += RowHeight(row);
        }
        while (used > space && sScroll < sRow) {
            used -= RowHeight(sScroll++);
        }
        if (sScroll > 0) {
            Text(left + panelW - 30, y - 12, 1, "^", 200, 190, 150);
        }
    }
    for (row = sScroll; row < sRowCounts[sPage] && y + RowHeight(row) <= bottom; row++) {
        char value[96], label[64];
        int selected = row == sRow;
        int locked = sPage == PAGE_OPTIONS &&
                     ((row == OPT_FPS && sContext.online) || (row == OPT_SCALE && !sContext.vulkan));
        int binding = sPage == PAGE_CONTROLS && row >= CTL_BIND && row < CTL_BACK;
        if (sPage == PAGE_CONTROLS && row == CTL_BIND) {
            DrawTableHeader(left, y);
            y += TABLE_HEADER_H;
        }
        if (selected) {
            Fill(left + 10, y - 5, binding ? CELL_X0 - 14 : panelW - 20, 18, 120, 30, 22, 255);
        }
        Text(left + 22, y, 1, RowLabel(row, label, sizeof(label)), selected ? 255 : 210, selected ? 245 : 200,
             selected ? 230 : 185);
        if (binding) {
            DrawBindingRow(left, y, row, selected);
        } else if (sPage == PAGE_OPTIONS || sPage == PAGE_CONTROLS) {
            if (sPage == PAGE_OPTIONS) {
                OptionValue(row, value, sizeof(value));
            } else {
                ControlValue(row, value, sizeof(value));
            }
            if (value[0]) {
                char shown[110];
                snprintf(shown, sizeof(shown), selected && !locked ? "< %s >" : "  %s", value);
                Text(left + (wide ? 190 : 160), y, 1, shown, locked ? 130 : 240, locked ? 125 : 210,
                     locked ? 120 : 150);
            }
        }
        y += ROW_H;
    }
    if (row < sRowCounts[sPage]) {
        Text(left + panelW - 30, bottom - 4, 1, "v", 200, 190, 150);
    }
    hint = sPage == PAGE_WAIT   ? "Hold Back (Backspace) for 2 seconds to leave"
           : sPage == PAGE_CODE ? "Type letters, or up/down and right   Enter: join"
           : sCaptureSince      ? (sCol < INPUT_PAD_SLOTS ? "Press a gamepad button or push a stick   Esc: cancel   Del: clear"
                                                          : "Press a key or click a mouse button   Esc: cancel   Del: clear")
           : sPage == PAGE_CONTROLS && sRow >= CTL_BIND && sRow < CTL_BACK
               ? "Enter/A: bind   Del/Y: clear   Arrows: choose   Hold Back: go back"
               : "Arrows: choose   Enter: select   Hold Back: go back";
    Text(left + 20, top + panelH - 22, 1, hint, 150, 140, 125);
}

/* The corner's lines when the menu is shut: the frame counter, a wait or a match, a message. */
static void DrawHud(void)
{
    float y = 4;
    if (g_settings.showFps && sFpsShown >= 0) {
        char text[32];
        snprintf(text, sizeof(text), "%d fps", sFpsShown);
        Fill(4, y, 8 * (float)strlen(text) + 8, 16, 0, 0, 0, 160);
        Text(8, y + 4, 1, text, 120, 255, 120);
        y += 20;
    }
    if (sBanner[0]) {
        Fill(4, y, 8 * (float)strlen(sBanner) + 8, 16, 0, 0, 0, 160);
        Text(8, y + 4, 1, sBanner, 235, 220, 190);
        y += 20;
    }
    if (sToast[0]) {
        Fill(4, y, 8 * (float)strlen(sToast) + 8, 16, 60, 10, 6, 200);
        Text(8, y + 4, 1, sToast, 255, 230, 200);
    }
}

static void DrawBackHold(void)
{
    const float w = 260, h = 26, x = (OVERLAY_WIDTH - w) / 2, y = OVERLAY_HEIGHT - 46;
    const char *what = sOpen ? (sPage == PAGE_WAIT ? "Leaving..." : "Going back...")
                       : OnlineInMatch() ? "Leaving the match..." : "Stopping watching...";
    Fill(x, y, w, h, 0, 0, 0, 200);
    Fill(x + 3, y + 3, (w - 6) * (float)sBackShown / 100, h - 6, 170, 40, 30, 255);
    Text(x + (w - 8 * (float)strlen(what)) / 2, y + 9, 1, what, 255, 240, 220);
}

/* Each picture: the hold on Back, the online state's turns, messages, the banner. */
static void Update(void)
{
    OnlineState online = OnlineGetState();
    const char *message = OnlineTakeMessage();
    char banner[160];
    Uint64 now = SDL_GetTicksNS();

    if (message) {
        Toast(message);
    }
    {
        /* A fight to watch began: the menu gets out of the way. */
        static int wasWatching;
        int watching = OnlineWatching();
        if (watching && !wasWatching) {
            SetOpen(0);
        }
        wasWatching = watching;
    }
    if (online != sLastOnline) {
        if (online == ONLINE_PLAYING) {
            SetOpen(0); /* the match begins */
        } else if (sLastOnline == ONLINE_PLAYING) {
            SetOpen(1); /* the match ended */
            Go(online == ONLINE_WAITING ? PAGE_WAIT : PAGE_ONLINE);
        } else if (online == ONLINE_IDLE && sOpen && sPage == PAGE_WAIT) {
            Go(PAGE_ONLINE); /* the wait ended without a match */
        } else if (online == ONLINE_SETUP && sOpen) {
            Go(PAGE_WAIT);
        }
        sLastOnline = online;
        sDirty = 1;
    }
    if (sToast[0] && now > sToastUntil) {
        sToast[0] = 0;
        sDirty = 1;
    }
    OnlineBanner(banner, sizeof(banner));
    if (strcmp(banner, sBanner) != 0) {
        snprintf(sBanner, sizeof(sBanner), "%s", banner);
        sDirty = 1;
    }
    if (sCaptureSince && SDL_GetTicksNS() - sCaptureSince > (Uint64)CAPTURE_MS * 1000000ull) {
        sCaptureSince = 0; /* nothing came: the binding stays as it was */
        sDirty = 1;
    }
    if (sBackSince && !sBackDone) {
        /* With the menu open the hold goes back; in a match it leaves it; watching, it stops watching; otherwise
         * Back is the game's alone. */
        int counts = sOpen || OnlineInMatch() || OnlineWatching();
        int percent = counts ? (int)((now - sBackSince) / 1000000ull * 100 / BACK_HOLD_MS) : 0;
        if (percent >= 100) {
            sBackDone = 1;
            sBackShown = 0;
            if (sOpen) {
                Back();
            } else if (OnlineInMatch()) {
                OnlineLeave();
            } else if (OnlineWatching()) {
                OnlineStopWatching();
                Toast("Stopped watching. Turn it on again on the waiting page (F1).");
            }
            sDirty = 1;
        } else if (percent >= 8 && percent != sBackShown) {
            sBackShown = percent;
            sDirty = 1;
        }
    }
    if (sOpen && sPage == PAGE_CONTROLS) {
        /* gamepads come and go, and haptics start, while the page is up */
        static char last[160];
        char now[160];
        ControlValue(CTL_PAD1, now, 80);
        ControlValue(CTL_HAPTICS, now + strlen(now), 80);
        ControlValue(CTL_PAD2, now + strlen(now), 160 - strlen(now));
        if (strcmp(now, last) != 0) {
            snprintf(last, sizeof(last), "%s", now);
            sDirty = 1;
        }
    }
    if (sOpen && sPage != PAGE_MAIN && sPage != PAGE_OPTIONS && sPage != PAGE_CONTROLS) {
        /* the status line changes as the server answers */
        static char lastStatus[200];
        char status[200];
        OnlineDescribe(status, sizeof(status));
        if (strcmp(status, lastStatus) != 0) {
            snprintf(lastStatus, sizeof(lastStatus), "%s", status);
            sDirty = 1;
        }
    }
}

const uint32_t *OverlayPicture(int *changed)
{
    int show;
    Update();
    show = sOpen || g_settings.showFps || sBanner[0] || sToast[0] || sBackShown;
    *changed = 0;
    if (!sDraw || !show) {
        if (sShown) {
            sShown = 0;
            *changed = 1;
        }
        return NULL;
    }
    if (sDirty || !sShown) {
        SDL_SetRenderDrawBlendMode(sDraw, SDL_BLENDMODE_NONE);
        SDL_SetRenderDrawColor(sDraw, 0, 0, 0, 0);
        SDL_RenderClear(sDraw);
        SDL_SetRenderDrawBlendMode(sDraw, SDL_BLENDMODE_BLEND);
        if (sOpen) {
            DrawMenu();
        } else {
            DrawHud();
        }
        if (sBackShown) {
            DrawBackHold();
        }
        SDL_FlushRenderer(sDraw);
        sDirty = 0;
        sShown = 1;
        *changed = 1;
    }
    return (const uint32_t *)sCanvas->pixels;
}
