/* The context switch. Thirteen instructions, and the whole reason PicoOS
 * can run more than one thing at a time.
 *
 *     void switch_ctx(u32 *save_esp, u32 new_esp);
 *
 * The trick is that a task's entire state lives on its own stack. We push
 * the four registers the C calling convention says a function must
 * preserve, remember where the stack pointer ended up, then point the CPU
 * at another task's stack and pop the same four back. The return address
 * that `ret` finds belongs to the *other* task, so we return into a
 * different task than the one we were called from.
 *
 * Flags are deliberately not saved. Every switch happens with interrupts
 * already disabled, and each task re-enables them the natural way: either
 * by returning out of the interrupt that stopped it (iret restores the old
 * flags), or, for a brand new task, with an explicit sti in the trampoline.
 */
    .code32
    .text
    .globl switch_ctx
    .type  switch_ctx, @function
switch_ctx:
    movl    4(%esp), %eax           /* where to store the old esp */
    movl    8(%esp), %edx           /* the stack to switch to     */

    pushl   %ebp
    pushl   %ebx
    pushl   %esi
    pushl   %edi

    movl    %esp, (%eax)            /* the old task is now fully on its stack */
    movl    %edx, %esp              /* ... and we are standing on the new one */

    popl    %edi
    popl    %esi
    popl    %ebx
    popl    %ebp
    ret                             /* returns into the new task */
