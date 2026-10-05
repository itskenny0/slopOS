/* PicoOS DOOM port: <math.h>. Doom's fixed-point code needs no libm;
 * the one atan call sits inside #if 0. fabs is the only real call. */
#ifndef _PICO_MATH_H
#define _PICO_MATH_H

double fabs(double x);

#endif
