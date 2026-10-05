/*  rtc.c -- the clock on the motherboard.
 *
 *  The desktop wanted to show the time and there was no way to ask for it:
 *  the kernel knows how many ticks have gone by since it started and
 *  nothing at all about what time of day it is. Every PC has had an answer
 *  to that since 1984, sitting in fourteen bytes of CMOS kept alive by a
 *  coin cell, and asking it costs two I/O ports.
 *
 *  The one argument worth having with the firmware is the format. Register
 *  B says which of two it picked: the clock can count in binary or in BCD,
 *  and it can count to twelve or to twenty four. Believe neither default
 *  and read what it says.
 */

#include "pico.h"

#define CMOS_ADDR   0x70
#define CMOS_DATA   0x71

#define REG_SEC     0x00
#define REG_MIN     0x02
#define REG_HOUR    0x04
#define REG_STAT_A  0x0A
#define REG_STAT_B  0x0B

static u8 cmos_read(u8 reg)
{
    outb(CMOS_ADDR, (u8)(reg | 0x80));      /* top bit: leave NMI alone */
    return inb(CMOS_DATA);
}

/* While the clock is ticking over, the bytes are briefly not a time at all.
 * Register A says when it is busy; wait for it rather than reading a
 * half-updated minute -- and give up waiting, because a machine with a
 * flat battery or no RTC at all would hang here forever otherwise. */
static void wait_ready(void)
{
    for (int i = 0; i < 1000; i++)
        if (!(cmos_read(REG_STAT_A) & 0x80)) return;
}

static int from_bcd(u8 v, int bcd_mode)
{
    return bcd_mode ? (int)(v & 0x0F) + (int)(v >> 4) * 10 : (int)v;
}

void rtc_time(int *hour, int *min, int *sec)
{
    wait_ready();

    u8  b    = cmos_read(REG_STAT_B);
    int bcdm = !(b & 0x04);                 /* bit 2 clear: BCD, not binary */
    int h    = from_bcd(cmos_read(REG_HOUR), bcdm);
    int m    = from_bcd(cmos_read(REG_MIN),  bcdm);
    int s    = from_bcd(cmos_read(REG_SEC),  bcdm);

    if (b & 0x02) {                         /* bit 1: twelve hour mode */
        int pm = h & 0x80;                  /* and bit 7 is the pm flag */
        h &= 0x7F;
        if (pm && h != 12) h += 12;
        if (!pm && h == 12) h = 0;
    }

    if (h > 23) h = 0;
    if (m > 59) m = 0;
    if (s > 59) s = 0;

    if (hour) *hour = h;
    if (min)  *min  = m;
    if (sec)  *sec  = s;
}

/* ---- setting the clock (new in 1.0) --------------------------------
 *
 * The chip is happy to be written to, but not while it is updating itself.
 * Register B bit 7 is the official pause button: set it, write the bytes,
 * clear it again. That is the whole dance, and it is the same one the
 * firmware setup screens have been doing since 1984.
 *
 * Values have to be written in whatever format register B says the clock
 * is counting in -- binary or BCD -- or the clock will happily run from
 * a nonsense base and be wrong forever in a very consistent way. */

static void cmos_write(u8 reg, u8 v)
{
    outb(CMOS_ADDR, (u8)(reg | 0x80));      /* top bit: leave NMI alone */
    outb(CMOS_DATA, v);
}

static u8 to_bcd(int v, int bcd_mode)
{
    if (!bcd_mode) return (u8)v;
    return (u8)((v / 10) << 4 | (v % 10));
}

void rtc_set_time(int hour, int min, int sec)
{
    if (hour < 0 || hour > 23 || min < 0 || min > 59 || sec < 0 || sec > 59)
        return;

    u8 b = cmos_read(REG_STAT_B);
    int bcdm = !(b & 0x04);

    cmos_write(REG_STAT_B, b | 0x80);       /* stop the update cycle  */
    cmos_write(REG_SEC,  to_bcd(sec,  bcdm));
    cmos_write(REG_MIN,  to_bcd(min,  bcdm));
    cmos_write(REG_HOUR, to_bcd(hour, bcdm));
    cmos_write(REG_STAT_B, b & 0x7F);       /* and let it run again   */
}

void rtc_date(int *year, int *month, int *day)
{
    wait_ready();

    u8  b    = cmos_read(REG_STAT_B);
    int bcdm = !(b & 0x04);

    int y  = from_bcd(cmos_read(0x09), bcdm);      /* year, two digits  */
    int c  = from_bcd(cmos_read(0x32), bcdm);      /* century, if there */
    int mo = from_bcd(cmos_read(0x08), bcdm);
    int d  = from_bcd(cmos_read(0x07), bcdm);

    /* No century register (a 386 often does not have one): pick the
     * century the sane way. An RTC that says year 80 is not from 2080. */
    if (c >= 19 && c <= 21) y += c * 100;
    else if (y < 80)        y += 2000;
    else                    y += 1900;

    if (mo < 1 || mo > 12) mo = 1;
    if (d  < 1 || d  > 31) d  = 1;

    if (year)  *year  = y;
    if (month) *month = mo;
    if (day)   *day   = d;
}

void rtc_set_date(int year, int month, int day)
{
    if (month < 1 || month > 12 || day < 1 || day > 31) return;
    if (year < 1970 || year > 2099) return;

    u8 b = cmos_read(REG_STAT_B);
    int bcdm = !(b & 0x04);

    int y = year % 100;

    cmos_write(REG_STAT_B, b | 0x80);       /* stop the update cycle  */
    cmos_write(0x07, to_bcd(day,   bcdm));
    cmos_write(0x08, to_bcd(month, bcdm));
    cmos_write(0x09, to_bcd(y,     bcdm));
    if (bcdm) cmos_write(0x32, to_bcd(year / 100, 1));
    else      cmos_write(0x32, (u8)(year / 100));
    cmos_write(REG_STAT_B, b & 0x7F);       /* and let it run again   */
}
