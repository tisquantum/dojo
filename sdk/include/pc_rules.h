/* The port's optional rules (port/src/substep.c holds them): changes to the game the player turns on, each off
 * by default, so that off is the console's game exactly. The game's code tests them under TARGET_PC.
 *
 * They change the simulation, so both machines of an online match must agree: each holds there only when both
 * players have it on (online.c), and replays and watched matches carry them. Offline they follow the settings
 * (host.c). */

#ifndef PC_RULES_H
#define PC_RULES_H

#define PC_RULE_FIX_BUGS 1 /* the original game's bugs fixed (docs/findings.md has each) */
#define PC_RULE_EXTRAS   2 /* what the disc holds that play never reached: Stage 18, the two-player Continue */
#define PC_RULE_JP_BLOOD 4 /* the Japanese release's semi-transparent amber blood */
#define PC_RULE_ALL      7

extern int g_pcRules;

#define PC_FIX_BUGS (g_pcRules & PC_RULE_FIX_BUGS)
#define PC_EXTRAS   (g_pcRules & PC_RULE_EXTRAS)
#define PC_JP_BLOOD (g_pcRules & PC_RULE_JP_BLOOD)

#endif
