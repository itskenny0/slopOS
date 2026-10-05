/* unpack.c -- the other half of tools/lzss.py
 *
 * Reads the LZSS stream that the build tools produce. There is no window
 * buffer and no scratch allocation: back-references point into the output
 * we have already written, so the only memory this needs is the destination
 * itself. That is deliberate. Unpacking happens on machines where a 4 KB
 * ring buffer is a real fraction of the free heap.
 *
 * The decoder is hostile to malformed input on purpose. An archive can
 * arrive corrupt, and a back-reference that points before the start of the
 * output is an invitation to read whatever is in front of the buffer. Every
 * offset is checked against how much has actually been written, and the
 * writer never goes past `max`. A bad stream costs you the file, not the
 * machine.
 */

#include "pico.h"

#define MINLEN 3

/* Returns the number of bytes written, or -1 if the stream is malformed.
 * Copies byte by byte rather than with memcpy: overlapping references are
 * not a bug here, they are how a run of the same byte is encoded (offset 1,
 * length 18 is nineteen bytes of the same value), and memcpy would get the
 * overlap wrong. */
int unpack(const u8 *src, u32 srclen, u8 *dst, u32 max)
{
    u32 i = 0, o = 0;

    while (o < max) {
        if (i >= srclen) return -1;
        u8 flags = src[i++];

        for (int bit = 0; bit < 8 && o < max; bit++) {
            if (flags & (1 << bit)) {
                if (i >= srclen) return -1;
                dst[o++] = src[i++];
            } else {
                if (i + 1 >= srclen) return -1;
                u32 b0 = src[i], b1 = src[i + 1];
                i += 2;
                u32 off = b0 | ((b1 >> 4) << 8);
                u32 len = (b1 & 0x0F) + MINLEN;

                if (off == 0 || off > o) return -1;
                if (o + len > max) len = max - o;

                const u8 *from = dst + o - off;
                while (len--) dst[o++] = *from++;
            }
        }
    }
    return (int)o;
}
