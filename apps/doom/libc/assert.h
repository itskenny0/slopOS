/* PicoOS DOOM port: <assert.h>. Doom builds with NDEBUG; asserts vanish. */
#ifndef _PICO_ASSERT_H
#define _PICO_ASSERT_H

#ifdef NDEBUG
#define assert(x) ((void)0)
#else
#include <stdio.h>
#include <stdlib.h>
#define assert(x) ((x) ? (void)0 : (printf("assert: %s:%d: %s\n", __FILE__, __LINE__, #x), abort()))
#endif

#endif
