/* PicoOS DOOM port: minimal <sys/types.h>. */
#ifndef _PICO_SYS_TYPES_H
#define _PICO_SYS_TYPES_H

#ifndef PICOAPP_H
typedef __SIZE_TYPE__ size_t;
#endif
typedef long ssize_t;
typedef int off_t;
typedef long time_t;
typedef unsigned int mode_t;
typedef int pid_t;
typedef unsigned int uid_t;
typedef unsigned int gid_t;
typedef long clock_t;

#endif
