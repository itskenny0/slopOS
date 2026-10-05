/* PicoOS DOOM port: <strings.h>. */
#ifndef _PICO_STRINGS_H
#define _PICO_STRINGS_H

#include <stddef.h>

int strcasecmp(const char *a, const char *b);
int strncasecmp(const char *a, const char *b, size_t n);

#endif
