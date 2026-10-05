/* task.c -- pre-emptive round-robin multitasking
 *
 * PicoOS runs several things at once without a memory manager, without user
 * mode and without locks. That sounds reckless, and on a big OS it would be.
 * Here it works because of three deliberate rules:
 *
 *   1. Every task is kernel code sharing one address space. A switch is
 *      therefore just a stack swap -- no page tables, no TSS, no ring
 *      change. That is why it costs thirteen instructions and why it is
 *      fast enough on a 386 at 4 MHz.
 *
 *   2. Exactly one task at a time owns the screen and the keyboard. That
 *      task is the foreground. Everything a background task prints goes
 *      into a small ring buffer attached to that task, and is replayed when
 *      you bring it forward. No windowing, no locking, no interleaved mess.
 *
 *   3. Anything that can be interrupted halfway and leave a mess -- the
 *      heap, the screen -- runs with interrupts off for the few
 *      instructions that matter. Cheaper than a mutex and, with one CPU,
 *      just as correct.
 *
 * A task costs a struct plus a 32 KB stack, both taken from the heap, so an
 * idle PicoOS pays nothing for a feature it is not using. 32 KB because
 * DOOM's renderer nests deeper than a shell ever does.
 */

#include "pico.h"

#define STACK_SZ   32768
#define OUTBUF     256          /* what a background task said, most recent */
#define QUANTUM    2            /* timer ticks before we force a switch     */
#define PAINT      0xB5B5B5B5u  /* same stack paint the boot stack uses     */

enum { T_FREE = 0, T_READY, T_ZOMBIE };

struct task {
    u32   esp;                  /* only meaningful while not running */
    u8   *stack;                /* NULL for the shell: it owns the boot stack */
    void *image;                /* .pico image to hand back on exit  */
    void *argblk;               /* copied argv block, likewise       */
    void (*fn)(void *);
    void *arg;

    int   pid;
    u8    state;
    u8    fg;                   /* owns screen + keyboard            */
    u8    killme;               /* asked to stop at the next chance  */
    u8    waiting;              /* parked on an event; idle if all are */

    int   exit_code;
    u32   born, cpu;            /* ticks */

    u32   jb[6];                /* fault escape hatch, one per task  */

    u8    color;                /* its own text colour, see below    */
    u16   outlen;
    u8    outwrap;
    char  out[OUTBUF];

    char  name[FS_NAMELEN];
};

static struct task *tasks[TASK_MAX];
static struct task *cur = NULL;
static int  next_pid  = 1;
static int  mt        = 0;      /* 0 until task_init runs */
static int  resched   = 0;
static u32  quantum   = 0;

extern void switch_ctx(u32 *save_esp, u32 new_esp);

/* ------------------------------------------------------------ helpers -- */

static int slot_of(struct task *t)
{
    for (int i = 0; i < TASK_MAX; i++) if (tasks[i] == t) return i;
    return 0;
}

/* Pick the next runnable task and go there. Interrupts must already be off.
 * Returns 1 if we actually switched. */
static int do_switch(void)
{
    if (!mt || !cur) return 0;

    int n = slot_of(cur);
    struct task *nx = NULL;
    for (int k = 1; k <= TASK_MAX; k++) {
        struct task *t = tasks[(n + k) % TASK_MAX];
        if (t && t->state == T_READY) { nx = t; break; }
    }
    if (!nx || nx == cur) return 0;

    struct task *old = cur;
    cur = nx;
    quantum = 0;
    switch_ctx(&old->esp, nx->esp);
    return 1;                    /* we are back, possibly much later */
}

/* Give up the rest of our slice. Safe to call from anywhere. */
void task_yield(void)
{
    u32 f = irq_save();
    int did = do_switch();
    irq_restore(f);
    if (!did && (f & 0x200)) hlt();     /* nothing else to do: save power */
}

/* Park until something happens (a key, the clock). Identical to yield
 * except that when *everybody* is parked we let the CPU sleep, which is
 * what keeps an idle PicoOS at almost no power. */
void task_wait(void)
{
    if (!mt) { hlt(); return; }

    cur->waiting = 1;
    int busy = 0;
    for (int i = 0; i < TASK_MAX; i++) {
        struct task *t = tasks[i];
        if (t && t->state == T_READY && !t->waiting) busy = 1;
    }
    if (!busy) hlt();
    else       task_yield();
    cur->waiting = 0;
}

/* ------------------------------------------------------------ the clock --
 * Called from the timer interrupt. We only raise a flag here: the actual
 * switch happens at the very end of the interrupt, after the PIC has been
 * told the interrupt is over. Switching before that would leave the timer
 * masked inside whichever task we jumped to, and the machine would freeze
 * on the spot. */
void task_tick(void)
{
    if (!mt || !cur) return;
    /* Only bill a task for the tick if it was actually doing something.
     * A task parked in task_wait() is still "current" when the clock
     * fires, and charging it made an idle machine report 100% busy in
     * the task manager -- two tasks doing nothing, 50% each. */
    if (!cur->waiting) cur->cpu++;
    if (++quantum >= QUANTUM) resched = 1;
}

/* Called at the tail of the interrupt dispatcher, interrupts still off. */
void task_resched(void)
{
    if (!mt || !cur) return;

    /* A task that was asked to die is stopped here, even if it never calls
     * into the kernel again -- that is what makes `kill` work on a program
     * stuck in a loop. The jump lands in the wrapper in exec.c, which
     * cleans up and switches away for good. */
    if (cur->killme && cur->image) {
        cur->killme = 0;
        pico_longjmp(cur->jb, 4);
    }
    if (!resched) return;
    resched = 0;
    do_switch();
}

/* --------------------------------------------------------------- birth -- */

/* Where a fresh task starts life. switch_ctx `ret`s straight into here. */
static void trampoline(void)
{
    sti();                       /* we arrived with interrupts off */
    cur->fn(cur->arg);
    task_exit(0);                /* if the body just returns */
}

int task_create(const char *name, void (*fn)(void *), void *arg,
                void *image, void *argblk, int foreground)
{
    if (!mt) return -1;

    int slot = -1;
    for (int i = 0; i < TASK_MAX; i++) if (!tasks[i]) { slot = i; break; }
    if (slot < 0) return -2;                       /* task table full */

    struct task *t = (struct task *)kmalloc(sizeof *t);
    if (!t) return -1;
    u8 *stk = (u8 *)kmalloc(STACK_SZ);
    if (!stk) { kfree(t); return -1; }

    memset(t, 0, sizeof *t);
    for (u32 i = 0; i < STACK_SZ / 4; i++) ((u32 *)stk)[i] = PAINT;

    t->stack  = stk;
    t->image  = image;
    t->argblk = argblk;
    t->fn     = fn;
    t->arg    = arg;
    t->pid    = next_pid++;
    t->state  = T_READY;
    t->born   = timer_ticks();
    t->color  = con_getcolor();     /* start with whatever the shell had */
    strncpy(t->name, name, FS_NAMELEN - 1);

    /* Build the stack the way switch_ctx expects to find it: four saved
     * registers with a return address sitting just above them. */
    u32 *sp = (u32 *)(stk + STACK_SZ);
    *--sp = 0;                       /* a landing pad for the trampoline  */
    *--sp = (u32)trampoline;         /* what `ret` will jump to           */
    *--sp = 0;                       /* ebp */
    *--sp = 0;                       /* ebx */
    *--sp = 0;                       /* esi */
    *--sp = 0;                       /* edi */
    t->esp = (u32)sp;

    u32 f = irq_save();
    tasks[slot] = t;
    if (foreground) { for (int i = 0; i < TASK_MAX; i++) if (tasks[i]) tasks[i]->fg = 0; t->fg = 1; }
    irq_restore(f);
    return t->pid;
}

/* Called by the task itself when it is finished. Never returns. */
void task_exit(int code)
{
    if (!mt || !cur) for (;;) hlt();

    cur->exit_code = code;
    if (cur->image)  { kfree(cur->image);  cur->image  = NULL; }
    if (cur->argblk) { kfree(cur->argblk); cur->argblk = NULL; }

    cli();
    cur->state = T_ZOMBIE;
    if (cur->fg) {                          /* hand the screen back */
        cur->fg = 0;
        for (int i = 0; i < TASK_MAX; i++)
            if (tasks[i] && tasks[i]->state == T_READY) { tasks[i]->fg = 1; break; }
    }
    for (;;) { do_switch(); sti(); hlt(); cli(); }   /* we are done */
}

/* --------------------------------------------------------- bookkeeping -- */

static void flush_log(struct task *t);

static struct task *find(int pid)
{
    for (int i = 0; i < TASK_MAX; i++)
        if (tasks[i] && tasks[i]->pid == pid) return tasks[i];
    return NULL;
}

int task_done(int pid)
{
    struct task *t = find(pid);
    return !t || t->state == T_ZOMBIE;
}

int task_exit_code(int pid)
{
    struct task *t = find(pid);
    return t ? t->exit_code : 0;
}

/* Release a finished task's stack and struct.
 *
 * Reaping a named task is silent -- whoever asked was waiting for it and
 * has the exit code already. Reaping everything (what the shell does at
 * each prompt) announces the ones that finished behind your back, and
 * replays whatever they managed to say, so nothing disappears unnoticed. */
void task_reap(int pid)
{
    for (;;) {
        u32 f = irq_save();
        struct task *t = NULL;
        int slot = -1;
        for (int i = 0; i < TASK_MAX; i++) {
            if (!tasks[i] || tasks[i]->state != T_ZOMBIE) continue;
            if (pid && tasks[i]->pid != pid) continue;
            t = tasks[i]; slot = i; break;
        }
        if (!t) { irq_restore(f); return; }
        tasks[slot] = NULL;
        irq_restore(f);

        if (!pid) {
            con_setcolor(DGREY, BLACK);
            if (t->exit_code == -100)
                kprintf("  [%d] %s crashed\n", t->pid, t->name);
            else if (t->exit_code == -101)
                kprintf("  [%d] %s stopped\n", t->pid, t->name);
            else
                kprintf("  [%d] %s finished (exit %d)\n",
                        t->pid, t->name, t->exit_code);
            con_setcolor(LGREY, BLACK);
            flush_log(t);
        }
        /* Whatever the program asked for and never handed back dies with
         * it -- a browser that leaks its page buffer would eat the machine
         * one page at a time otherwise. */
        mem_free_task(t->pid);
        kfree(t->stack);
        kfree(t);
    }
}

int task_kill(int pid)
{
    struct task *t = find(pid);
    if (!t || t->state != T_READY) return -1;
    if (!t->image) return -2;               /* the shell is not killable */
    t->killme = 1;
    return 0;
}

/* The keyboard interrupt calls this on ctrl-c. */
int task_sigint(void)
{
    if (!mt || !cur) return 0;
    for (int i = 0; i < TASK_MAX; i++) {
        struct task *t = tasks[i];
        if (t && t->fg && t->image && t->state == T_READY) { t->killme = 1; return 1; }
    }
    return 0;
}

int task_self(void)      { return cur ? cur->pid : 0; }
int task_stack_size(void) { return STACK_SZ; }
u32 *task_jmpbuf(void)   { return cur ? cur->jb : NULL; }
int task_is_prog(void)   { return cur && cur->image ? 1 : 0; }
void task_set_exit(int c){ if (cur) cur->exit_code = c; }
int task_get_exit(void)  { return cur ? cur->exit_code : 0; }

const char *task_name(void)
{
    return (cur && cur->image) ? cur->name : NULL;
}

/* -------------------------------------------------------------- output --
 * The foreground task writes straight to the screen. Everyone else writes
 * into their own little ring, and we replay it on `fg`. */

int task_owns_console(void)
{
    return !mt || !cur || cur->fg;
}

/* Text colour belongs to the task, not to the screen.
 *
 * It did not, and that was the bug behind the mess you get from typing
 * `help` while tetris plays in the background. Every drawing call was
 * already refused for a task that does not own the console -- putc, clear,
 * gotoxy, all of them checked. con_setcolor did not, because it draws
 * nothing: it only sets a variable. But that variable was global, so a
 * background tetris repainting its blocks was changing the colour that the
 * foreground shell drew its next character in. The output was in the right
 * place, in the wrong colour, several times a second.
 *
 * Giving each task its own byte fixes it at the root and costs one byte per
 * task. A background task can set its colour as often as it likes now; it
 * lands in the ring buffer with everything else it did. */
u8 *task_colorp(void)
{
    return (mt && cur) ? &cur->color : NULL;
}

void task_log(char c)
{
    if (!cur) return;
    if (cur->outlen >= OUTBUF) { cur->outlen = 0; cur->outwrap = 1; }
    cur->out[cur->outlen++] = c;
}

static void flush_log(struct task *t)
{
    if (!t->outlen && !t->outwrap) return;
    if (t->outwrap) {
        con_setcolor(DGREY, BLACK);
        puts("  ...\n");
        con_setcolor(LGREY, BLACK);
        for (u16 i = t->outlen; i < OUTBUF; i++) putc(t->out[i]);
    }
    for (u16 i = 0; i < t->outlen; i++) putc(t->out[i]);
    t->outlen = 0;
    t->outwrap = 0;
}

/* Hand the screen and keyboard to another task. */
int task_setfg(int pid)
{
    struct task *t = find(pid);
    if (!t || t->state != T_READY) return -1;
    u32 f = irq_save();
    for (int i = 0; i < TASK_MAX; i++) if (tasks[i]) tasks[i]->fg = 0;
    t->fg = 1;
    irq_restore(f);
    flush_log(t);
    return 0;
}

/* ------------------------------------------------------------- listing -- */

static u32 stack_free_of(struct task *t)
{
    if (!t->stack) return 0;
    u32 *p = (u32 *)t->stack;
    u32 n = 0;
    while (n < STACK_SZ / 4 && p[n] == PAINT) n++;
    return n * 4;
}

void task_list(void)
{
    kprintf("  PID  STATE   CPU     STACK      NAME\n");
    for (int i = 0; i < TASK_MAX; i++) {
        struct task *t = tasks[i];
        if (!t) continue;
        const char *st = t->state == T_ZOMBIE ? "done" :
                         t->fg               ? "fore" :
                         t->waiting          ? "idle" : "back";
        u32 ms = t->cpu * 10;
        if (t->stack)
            kprintf("  %d    %s  %u ms%s  %u/%u B  %s\n",
                    t->pid, st, ms, ms < 100 ? "  " : "",
                    STACK_SZ - stack_free_of(t), STACK_SZ, t->name);
        else
            kprintf("  %d    %s  %u ms%s  boot     %s\n",
                    t->pid, st, ms, ms < 100 ? "  " : "", t->name);
    }
}

/* Fill in one row of the task table for the task manager.
 *
 * Returns 0 once `index` is past the last live task, which lets a caller
 * walk the list without first asking how long it is -- and without holding
 * any lock across the walk, because each call is its own snapshot.
 *
 * Zombies are skipped. A task that has finished but not been reaped is an
 * artefact of the bookkeeping, not something the user started, and showing
 * it would mean explaining it. */
int task_info(int index, pico_task_t *out)
{
    u32 f = irq_save();
    int n = 0;

    for (int i = 0; i < TASK_MAX; i++) {
        struct task *t = tasks[i];
        if (!t || t->state != T_READY) continue;
        if (n++ != index) continue;

        out->pid        = t->pid;
        out->foreground = t->fg;
        out->is_program = t->image ? 1 : 0;
        out->cpu_ticks  = t->cpu;
        out->age_ticks  = timer_ticks() - t->born;
        out->stack_size = t->stack ? STACK_SZ : stack_size();
        out->waiting    = t->waiting;

        /* The same paint trick the boot stack uses: a task's stack is
         * filled with a known pattern when it is created, so the high
         * water mark is wherever the paint stops. */
        if (t->stack) {
            u32 used = 0;
            const u32 *p = (const u32 *)t->stack;
            u32 words = STACK_SZ / 4;
            u32 clean = 0;
            while (clean < words && p[clean] == PAINT) clean++;
            used = STACK_SZ - clean * 4;
            out->stack_used = used;
        } else {
            out->stack_used = stack_used();
        }

        memcpy(out->name, t->name, 16);
        out->name[15] = 0;

        irq_restore(f);
        return 1;
    }

    irq_restore(f);
    return 0;
}

int task_running_count(void)
{
    int n = 0;
    for (int i = 0; i < TASK_MAX; i++)
        if (tasks[i] && tasks[i]->state == T_READY) n++;
    return n;
}

/* --------------------------------------------------------------- start -- */

/* Wrap whatever is running right now -- the shell, on the boot stack --
 * into task 1, so that from here on there is no special case. */
void task_init(void)
{
    struct task *t = (struct task *)kmalloc(sizeof *t);
    if (!t) return;
    memset(t, 0, sizeof *t);
    t->pid   = next_pid++;
    t->state = T_READY;
    t->fg    = 1;
    t->color = con_getcolor();
    strcpy(t->name, "shell");
    tasks[0] = t;
    cur = t;
    mt = 1;
}
