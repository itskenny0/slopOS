/* PicoOS DOOM port: <stdlib.h>. */
#ifndef _PICO_STDLIB_H
#define _PICO_STDLIB_H

#include <stddef.h>

void *malloc(size_t n);
void  free(void *p);
void *calloc(size_t n, size_t size);
void *realloc(void *p, size_t n);

void  exit(int code);
void  abort(void);
int   atexit(void (*fn)(void));

int   atoi(const char *s);
long  atol(const char *s);
double atof(const char *s);
long  strtol(const char *s, char **end, int base);
unsigned long strtoul(const char *s, char **end, int base);
double strtod(const char *s, char **end);

int   abs(int x);
long  labs(long x);
int   rand(void);
void  srand(unsigned seed);

char *getenv(const char *name);
int   system(const char *cmd);

#define EXIT_SUCCESS 0
#define EXIT_FAILURE 1
#define RAND_MAX 0x7FFF

#endif
