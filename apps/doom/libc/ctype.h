/* PicoOS DOOM port: <ctype.h>. Plain functions, ASCII only. */
#ifndef _PICO_CTYPE_H
#define _PICO_CTYPE_H

int isalpha(int c);
int isalnum(int c);
int isdigit(int c);
int isxdigit(int c);
int isspace(int c);
int isupper(int c);
int islower(int c);
int isprint(int c);
int isgraph(int c);
int iscntrl(int c);
int ispunct(int c);
int toupper(int c);
int tolower(int c);

#endif
