/* The controls: each player's gamepad, the keyboard and mouse, the bindings from them to the console's
 * buttons (settings.h keeps them), and rumble, as motor rumble or a wired DualSense's haptics. */

#ifndef PORT_INPUT_H
#define PORT_INPUT_H

#include <stddef.h>
#include <SDL3/SDL.h>

/* A binding: what one slot of a console button is worked by, or 0 for nothing. */
#define INPUT_BIND_KEY(scancode) (0x10000 | (scancode))
#define INPUT_BIND_MOUSE(button) (0x20000 | (button))
#define INPUT_BIND_PAD(button)   (0x30000 | (button))
#define INPUT_BIND_AXIS(axis, negative) (0x40000 | ((axis) << 1) | (negative))
#define INPUT_BIND_KIND(binding) ((binding) & 0xF0000)

/* The slots of each button: two on a gamepad, then two on the keyboard and mouse. */
#define INPUT_PAD_SLOTS 2

/* The console's buttons, in the controls page's order: their names, dojo.ini keys and PadRead masks. */
typedef struct InputButton {
    const char *name;
    const char *key;
    unsigned mask;
} InputButton;
extern const InputButton INPUT_BUTTONS[];

void InputInit(void);
void InputShutdown(void);
/* A gamepad came or went, or a player's gamepad was chosen. */
void InputDevicesChanged(void);
/* Player `player`'s buttons (0 or 1), in PadRead's layout, from their gamepad and, if theirs, the keyboard. */
unsigned InputButtons(int player);

/* Puts button `button`'s bindings (or every button's, for -1) back to the defaults. */
void InputDefaultBindings(int button);
/* The binding an event makes for a gamepad slot (pad 1) or a keyboard-and-mouse slot (pad 0), or 0. */
int InputBindingFromEvent(const SDL_Event *e, int pad);
void InputBindingName(int binding, char *out, size_t size);

/* Player `player`'s gamepad: steps through auto, none and the connected gamepads; describes the choice. */
void InputStepPlayerPad(int player, int dir);
void InputDescribePlayerPad(int player, char *out, size_t size);
/* Whether haptics are in use now (a wired DualSense and its sound device), for the controls page. */
int InputHapticsActive(void);

/* Each field: the game's rumble to the players' gamepads; `running` 0 lets go of every motor. */
void InputUpdateRumble(int running);

/* The launcher's helpers, run as dojo.exe --pads (the gamepads and each player's), --capture-pad (the next
 * gamepad binding pressed) or --test-rumble 1|2; returns the exit code, or -1 for an unknown command. */
int InputTool(const char *command, const char *arg);

#endif
