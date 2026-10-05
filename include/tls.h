#ifndef TLS_H
#define TLS_H

/* tls.h -- the encrypted version of a socket
 *
 * `tls_open` shakes hands over a socket that is already connected and
 * leaves it encrypted; after that `tls_send` and `tls_read` are drop-in
 * replacements for the plain ones, and `tls_close` says goodbye and
 * hands the memory back. One session at a time, like the rest of the
 * stack. */
int  tls_open(int fd, const char *host, char *err, int errlen);
int  tls_send(const u8 *data, u32 len);
int  tls_read(u8 *out, u32 max, u32 timeout_ms);
void tls_close(void);
int  tls_active(void);

#endif
