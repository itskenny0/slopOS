/* PicoOS DOOM port: <stdio.h>. Files live in the kernel ramdisk; stdout and
 * stderr both print on the console. Implemented in pico/pico_libc.c. */
#ifndef _PICO_STDIO_H
#define _PICO_STDIO_H

#include <stddef.h>
#include <stdarg.h>

typedef struct PFILE FILE;

extern FILE *stdin;
extern FILE *stderr;
extern FILE *stdout;

#define EOF  (-1)
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

FILE *fopen(const char *path, const char *mode);
int   fclose(FILE *f);
size_t fread(void *ptr, size_t size, size_t nmemb, FILE *f);
size_t fwrite(const void *ptr, size_t size, size_t nmemb, FILE *f);
int   fseek(FILE *f, long offset, int whence);
long  ftell(FILE *f);
void  rewind(FILE *f);
int   fflush(FILE *f);
int   feof(FILE *f);
int   ferror(FILE *f);
void  clearerr(FILE *f);

int   fgetc(FILE *f);
int   getc(FILE *f);
int   getchar(void);
char *fgets(char *s, int n, FILE *f);
int   ungetc(int c, FILE *f);

int   fputc(int c, FILE *f);
int   putc(int c, FILE *f);
int   putchar(int c);
int   fputs(const char *s, FILE *f);
int   puts(const char *s);

int   printf(const char *fmt, ...);
int   fprintf(FILE *f, const char *fmt, ...);
int   vfprintf(FILE *f, const char *fmt, va_list ap);
int   snprintf(char *s, size_t n, const char *fmt, ...);
int   vsnprintf(char *s, size_t n, const char *fmt, va_list ap);
int   sprintf(char *s, const char *fmt, ...);
int   vsprintf(char *s, const char *fmt, va_list ap);
int   sscanf(const char *s, const char *fmt, ...);

int   remove(const char *path);
int   rename(const char *oldp, const char *newp);

#endif
