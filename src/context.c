/* The context switch between the host and the game (context.h), for 32-bit x86 Windows. A switch saves the
 * callee-saved registers and the thread block's exception-handler head and stack bounds on the current
 * stack, then restores the other context's, so exception dispatch sees the stack it runs on. */

#include <stdint.h>
#include <string.h>
#include "context.h"

#define GAME_STACK_SIZE (1 << 20)

static __declspec(align(16)) unsigned char sGameStack[GAME_STACK_SIZE];
static void *sHostSp;
static void *sGameSp;
static void (*sEntry)(void);

/* Saves the current context's stack pointer to *save and resumes the one whose is `resume`. */
__attribute__((naked)) static void SwitchStacks(void **save, void *resume)
{
    __asm__ volatile(
        "movl 4(%esp), %eax\n\t"
        "movl 8(%esp), %edx\n\t"
        "pushl %ebp\n\t"
        "pushl %ebx\n\t"
        "pushl %esi\n\t"
        "pushl %edi\n\t"
        "pushl %fs:0\n\t"
        "pushl %fs:4\n\t"
        "pushl %fs:8\n\t"
        "movl %esp, (%eax)\n\t"
        "movl %edx, %esp\n\t"
        "popl %fs:8\n\t"
        "popl %fs:4\n\t"
        "popl %fs:0\n\t"
        "popl %edi\n\t"
        "popl %esi\n\t"
        "popl %ebx\n\t"
        "popl %ebp\n\t"
        "ret\n\t");
}

static void GameStart(void)
{
    sEntry();
    for (;;) {
        ContextSwitchToHost();
    }
}

void ContextInit(void (*entry)(void))
{
    uint32_t *sp = (uint32_t *)(sGameStack + GAME_STACK_SIZE);
    sEntry = entry;
    /* The frame SwitchStacks pops on first entry: bounds and handler head, four registers, a return to
     * GameStart, and a slot standing for GameStart's own return address. */
    *--sp = 0;                                       /* GameStart's return address: it never returns */
    *--sp = (uint32_t)(uintptr_t)GameStart;          /* SwitchStacks' ret */
    *--sp = 0;                                       /* ebp */
    *--sp = 0;                                       /* ebx */
    *--sp = 0;                                       /* esi */
    *--sp = 0;                                       /* edi */
    *--sp = 0xFFFFFFFFu;                             /* fs:0, the end of the handler chain */
    *--sp = (uint32_t)(uintptr_t)(sGameStack + GAME_STACK_SIZE); /* fs:4, the stack's base */
    *--sp = (uint32_t)(uintptr_t)sGameStack;         /* fs:8, its limit */
    sGameSp = sp;
}

void ContextGameStack(void **low, void **high)
{
    *low = sGameStack;
    *high = sGameStack + GAME_STACK_SIZE;
}

void *ContextGameStackPointer(void)
{
    return sGameSp;
}

void ContextSetGameStackPointer(void *sp)
{
    sGameSp = sp;
}

void ContextSwitchToGame(void)
{
    SwitchStacks(&sHostSp, sGameSp);
}

void ContextSwitchToHost(void)
{
    SwitchStacks(&sGameSp, sHostSp);
}
