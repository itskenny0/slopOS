/* pico_mfixed.c -- fixed-point multiply/divide without 64-bit helpers.
 *
 * Upstream m_fixed.c does these in C with int64_t. On 32-bit x86 gcc
 * turns that into calls to __muldi3/__divdi3, which only exist in
 * libgcc -- and this program links with -nostdlib, so they do not.
 * A 386 divides 64 by 32 in hardware, so do it the way the original
 * DOS DOOM did: one imull/shrd for the multiply, one idiv for the
 * divide, with upstream's overflow guard kept verbatim.
 *
 * On anything but a 386 (the host test harness) plain C is used.
 *
 * This file is part of the PicoOS DOOM port, GPL-2.0-or-later.
 */

#include "doomtype.h"
#include "m_fixed.h"

#include <stdlib.h>
#include <limits.h>

fixed_t FixedMul(fixed_t a, fixed_t b)
{
#ifdef __i386__
    fixed_t r;

    /* edx:eax = a * b, then take bits 16..47 */
    __asm__ volatile("imull %2\n\t"
                     "shrdl $16, %%edx, %0"
                     : "=a" (r)
                     : "0" (a), "r" (b)
                     : "edx", "cc");
    return r;
#else
    return (fixed_t)(((int64_t)a * (int64_t)b) >> FRACBITS);
#endif
}

fixed_t FixedDiv(fixed_t a, fixed_t b)
{
    /* abs(INT_MIN) is still negative, so INT_MIN would slip through the
     * guard into an overflowing idiv -- the CPU faults where upstream C
     * merely truncates. Saturate it the same way the guard does. */
    if (a == INT_MIN)
        return (a ^ b) < 0 ? INT_MIN : INT_MAX;
    if ((abs(a) >> 14) >= abs(b))
        return (a ^ b) < 0 ? INT_MIN : INT_MAX;

#ifdef __i386__
    {
        fixed_t r;

        /* edx:eax = (int64)a << 16, then a single signed divide.
         * b is pinned to ecx: edx is the other half of the dividend,
         * so the compiler must not put the divisor there. */
        __asm__ volatile("movl %1, %%eax\n\t"
                         "movl %%eax, %%edx\n\t"
                         "sarl $16, %%edx\n\t"
                         "sall $16, %%eax\n\t"
                         "idivl %2"
                         : "=a" (r)
                         : "r" (a), "c" (b)
                         : "edx", "cc");
        return r;
    }
#else
    return (fixed_t)(((int64_t)a << 16) / b);
#endif
}
