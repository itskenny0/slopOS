/* PicoOS DOOM port: minimal <inttypes.h>. Doom only needs the types. */
#ifndef _PICO_INTTYPES_H
#define _PICO_INTTYPES_H

#include <stdint.h>

#define PRId32 "d"
#define PRIi32 "i"
#define PRIu32 "u"
#define PRIx32 "x"
#define PRIX32 "X"
#define PRId64 "lld"
#define PRIi64 "lli"
#define PRIu64 "llu"
#define PRIx64 "llx"

#endif
