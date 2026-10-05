/* A four-instruction setjmp/longjmp pair.
 *
 * Used for one thing only: when a .pico program faults, the exception
 * handler jumps back to the shell instead of halting the machine. There is
 * no paging and no user mode here, so a bad program can still corrupt the
 * kernel -- but the common case (a null dereference, a divide by zero, a
 * bad jump) is survivable, and that is the difference between "the OS
 * killed the program" and "the program killed the OS".
 *
 * The buffer holds ebx, esi, edi, ebp, esp and the return address.
 */
    .text
    .globl pico_setjmp
    .type  pico_setjmp, @function
pico_setjmp:
    movl    4(%esp), %ecx           /* the jmp_buf                      */
    movl    (%esp), %edx            /* our return address               */
    movl    %ebx,  0(%ecx)
    movl    %esi,  4(%ecx)
    movl    %edi,  8(%ecx)
    movl    %ebp, 12(%ecx)
    leal    4(%esp), %eax           /* esp as it will be after we return */
    movl    %eax, 16(%ecx)
    movl    %edx, 20(%ecx)
    xorl    %eax, %eax              /* the direct call returns 0        */
    ret

    .globl pico_longjmp
    .type  pico_longjmp, @function
pico_longjmp:
    movl    4(%esp), %ecx           /* the jmp_buf                      */
    movl    8(%esp), %eax           /* the value setjmp should return   */
    testl   %eax, %eax
    jnz     1f
    movl    $1, %eax                /* never hand back a 0              */
1:
    movl     0(%ecx), %ebx
    movl     4(%ecx), %esi
    movl     8(%ecx), %edi
    movl    12(%ecx), %ebp
    movl    16(%ecx), %esp
    movl    20(%ecx), %edx
    jmp     *%edx
