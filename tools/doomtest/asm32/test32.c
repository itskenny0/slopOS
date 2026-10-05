/* 32-bit asm check for pico_mfixed: compares the real FixedMul/FixedDiv
 * against 20000 precomputed vectors + edge cases. Own _start, int 0x80. */
typedef int fixed_t;
fixed_t FixedMul(fixed_t a, fixed_t b);
fixed_t FixedDiv(fixed_t a, fixed_t b);
int abs(int x) { return x < 0 ? -x : x; }
#include "vectors.h"

static void sys_exit(int c)
{
    __asm__ volatile("int $0x80" : : "a"(1), "b"(c));
}

void _start(void)
{
    int i;
    /* edge cases */
    if (FixedMul(0x00010000, 0x00010000) != 0x00010000) sys_exit(10);
    if (FixedDiv(0x00020000, 0x00030000) != 0x0000AAAA) sys_exit(11);
    if (FixedDiv(0x10000000, 0x00001000) != 0x7FFFFFFF) sys_exit(12);
    if (FixedDiv((fixed_t)0xF0000000, 0x00001000) != (fixed_t)0x80000000) sys_exit(13);
    if (FixedDiv(0x00010000, 0) != 0x7FFFFFFF) sys_exit(14);
    if (FixedDiv((fixed_t)0x80000000, (fixed_t)0xFFFFFFFF) != 0x7FFFFFFF) sys_exit(15);
    for (i = 0; i < 20000; i++) {
        if (FixedMul(V[i].a, V[i].b) != V[i].mul) sys_exit(20);
        if (FixedDiv(V[i].a, V[i].b) != V[i].div) sys_exit(21);
    }
    sys_exit(0);
}
