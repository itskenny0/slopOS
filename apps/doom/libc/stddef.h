/* PicoOS DOOM port: freestanding <stddef.h>. size_t/NULL match picoapp.h. */
#ifndef _PICO_STDDEF_H
#define _PICO_STDDEF_H

#ifndef NULL
#define NULL ((void *)0)
#endif

#ifndef PICOAPP_H
typedef __SIZE_TYPE__ size_t;
#endif

typedef __PTRDIFF_TYPE__ ptrdiff_t;

#define offsetof(t, m) ((unsigned int)&((t *)0)->m)

#endif
