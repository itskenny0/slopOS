/* PicoOS DOOM port: freestanding <limits.h> for 32-bit x86. */
#ifndef _PICO_LIMITS_H
#define _PICO_LIMITS_H

#define CHAR_BIT  8
#define SCHAR_MIN (-128)
#define SCHAR_MAX 127
#define UCHAR_MAX 255
#define CHAR_MIN  SCHAR_MIN
#define CHAR_MAX  SCHAR_MAX
#define SHRT_MIN  (-32768)
#define SHRT_MAX  32767
#define USHRT_MAX 65535
#define INT_MIN   (-2147483647 - 1)
#define INT_MAX   2147483647
#define UINT_MAX  4294967295u
#define LONG_MAX  __LONG_MAX__
#define LONG_MIN  (-__LONG_MAX__ - 1L)
#define ULONG_MAX (__LONG_MAX__ * 2UL + 1UL)
#define LLONG_MAX __LONG_LONG_MAX__
#define LLONG_MIN (-__LONG_LONG_MAX__ - 1LL)
#define ULLONG_MAX (__LONG_LONG_MAX__ * 2ULL + 1ULL)

#define PATH_MAX  256
#define FILENAME_MAX 256

#endif
