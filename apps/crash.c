/* crash.pico -- deliberately breaks, to show what the kernel does about it.
 *
 * PicoOS has no paging and no user mode, so it cannot stop a program from
 * scribbling over the kernel. What it can do is catch the CPU exception,
 * kill the program and drop you back at the shell instead of halting the
 * machine.
 *
 *   start crash        divide by zero      -> caught, program killed
 *   start crash ud     invalid opcode      -> caught, program killed
 *   start crash int3   breakpoint trap     -> caught, program killed
 *   start crash null   write to address 0  -> NOT caught, and that is the
 *                                             honest answer: without paging
 *                                             address 0 is just memory
 */
#include "picoapp.h"

int app_main(int argc, char **argv)
{
    const char *what = argc > 1 ? argv[1] : "div";

    api->setcolor(YELLOW, BLACK);
    api->printf("\n  about to fault on purpose (%s)...\n", what);
    api->setcolor(LGREY, BLACK);
    api->sleep(300);

    if (what[0] == 'n') {
        /* Computed at runtime so the compiler cannot fold it into a trap:
         * this really does store to physical address 0. */
        u32 addr = 0;
        __asm__ __volatile__("" : "+r"(addr));   /* gcc must not fold this */
        volatile u32 *p = (volatile u32 *)addr;
        u32 old = *p;
        *p = 0xDEADBEEF;
        int wrote = (*p == 0xDEADBEEF);
        *p = old;                             /* put it back, be polite */

        api->setcolor(LRED, BLACK);
        api->printf("  the write to address 0 %s\n",
                    wrote ? "succeeded" : "did nothing");
        api->setcolor(LGREY, BLACK);
        api->puts("  no paging means no memory protection -- a bad .pico\n"
                  "  can still corrupt the kernel. Only CPU faults are caught.\n");
        return 0;
    }

    if (what[0] == 'u') {                     /* invalid opcode  -> #UD */
        __asm__ __volatile__("ud2");
    } else if (what[0] == 'i') {              /* breakpoint      -> #BP */
        __asm__ __volatile__("int3");
    } else {                                  /* divide by zero  -> #DE */
        volatile int a = 1, b = 0;
        __asm__ __volatile__("cltd; idivl %1" : "+a"(a) : "r"(b) : "edx");
    }

    api->puts("  ...I survived, which should not happen\n");
    return 1;
}
