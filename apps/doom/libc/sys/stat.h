/* PicoOS DOOM port: <sys/stat.h>. The ramdisk is flat; mkdir always wins. */
#ifndef _PICO_SYS_STAT_H
#define _PICO_SYS_STAT_H

#include <sys/types.h>

struct stat {
    unsigned st_size;
    unsigned st_mode;
};

#define S_IFMT  0170000
#define S_IFREG 0100000
#define S_IFDIR 0040000

int mkdir(const char *path, ...);

#endif
