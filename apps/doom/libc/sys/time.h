/* PicoOS DOOM port: <sys/time.h>. Only the struct; timing is DG_GetTicksMs. */
#ifndef _PICO_SYS_TIME_H
#define _PICO_SYS_TIME_H

#include <sys/types.h>

struct timeval {
    long tv_sec;
    long tv_usec;
};

struct timezone {
    int tz_minuteswest;
    int tz_dsttime;
};

#endif
