/* The port's menus and prompts for the game's own screens (port/src/overlay.c draws them over the game's
 * picture). The game calls them every frame it wants them shown; they vanish when it stops. Not state: they
 * only draw. */

#ifndef PC_GAMEUI_H
#define PC_GAMEUI_H

/* A list of choices with the cursor on one. */
void PcGameMenu(const char *title, const char *const *items, int count, int cursor);
/* A bar that fills as a button is held, 0 to 100, with what the hold will do. */
void PcGameHold(int percent, const char *what);

#endif
