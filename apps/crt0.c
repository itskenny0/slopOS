/* crt0.c -- the first thing in every .pico image.
 *
 * The linker script puts .text.start at the very beginning, so the entry
 * point is always exactly PICO_LOAD_ADDR and the kernel never has to look
 * up a symbol. All this does is stash the api pointer and call app_main.
 */
#include "picoapp.h"

picoapi_t *api = NULL;

__attribute__((section(".text.start"), used))
int _start(picoapi_t *a, int argc, char **argv)
{
    api = a;
    return app_main(argc, argv);
}

/* gcc emits calls to these for struct copies and array initialisers even
 * with -ffreestanding, so a freestanding program has to supply them. */
void *memset(void *d, int c, size_t n)
{
    u8 *p = (u8 *)d;
    while (n--) *p++ = (u8)c;
    return d;
}

void *memcpy(void *d, const void *s, size_t n)
{
    u8 *a = (u8 *)d; const u8 *b = (const u8 *)s;
    while (n--) *a++ = *b++;
    return d;
}
