/* The two things the loader used gnu-efi's efilib for: the ST/BS globals
 * and Print. Print here knows %d %u %x %lx %s %c and %% -- everything
 * loader.c feeds it, and nothing it does not.
 */
#include "uefi.h"

/* Hidden on purpose: with -fpic the compiler reaches default-visibility
 * globals through the GOT, and a PE binary has no GOT to reach through --
 * the loader dies on startup jumping at garbage. Hidden globals are
 * accessed directly, which is all this loader needs. */
EFI_SYSTEM_TABLE *ST __attribute__((visibility("hidden")));
EFI_BOOT_SERVICES *BS __attribute__((visibility("hidden")));

EFI_STATUS InitializeLib(EFI_HANDLE image, EFI_SYSTEM_TABLE *systab)
{
    (void)image;
    ST = systab;
    BS = systab->BootServices;
    return EFI_SUCCESS;
}

static void emit(CHAR16 **o, CHAR16 *end, CHAR16 c)
{
    if (*o < end)
        *(*o)++ = c;
}

static void emit_u64(CHAR16 **o, CHAR16 *end, UINT64 v, int base, int upper)
{
    CHAR16 tmp[24];
    int n = 0;
    if (!v) {
        emit(o, end, '0');
        return;
    }
    while (v) {
        int d = (int)(v % (UINT64)base);
        tmp[n++] = (CHAR16)(d < 10 ? '0' + d : (upper ? 'A' : 'a') + d - 10);
        v /= (UINT64)base;
    }
    while (n--)
        emit(o, end, tmp[n]);
}

UINTN Print(CHAR16 *fmt, ...)
{
    __builtin_va_list ap;
    CHAR16 buf[512];
    CHAR16 *o = buf;
    CHAR16 *end = buf + 511;
    UINTN len;

    __builtin_va_start(ap, fmt);
    while (*fmt) {
        if (*fmt != '%') {
            emit(&o, end, *fmt++);
            continue;
        }
        fmt++;
        int longa = 0;
        while (*fmt == 'l') {
            longa = 1;
            fmt++;
        }
        if (*fmt == 'd' || *fmt == 'i') {
            INT64 v = longa ? __builtin_va_arg(ap, long)
                            : (INT64)__builtin_va_arg(ap, int);
            if (v < 0) {
                emit(&o, end, '-');
                v = -v;
            }
            emit_u64(&o, end, (UINT64)v, 10, 0);
            fmt++;
        } else if (*fmt == 'u') {
            UINT64 v = longa ? __builtin_va_arg(ap, unsigned long)
                             : (UINT64)__builtin_va_arg(ap, unsigned);
            emit_u64(&o, end, v, 10, 0);
            fmt++;
        } else if (*fmt == 'x' || *fmt == 'X' || *fmt == 'p') {
            UINT64 v = (*fmt == 'p') ? (UINT64)__builtin_va_arg(ap, void *)
                      : longa        ? __builtin_va_arg(ap, unsigned long)
                                     : (UINT64)__builtin_va_arg(ap, unsigned);
            emit_u64(&o, end, v, 16, *fmt == 'X');
            fmt++;
        } else if (*fmt == 's') {
            CHAR16 *s = __builtin_va_arg(ap, CHAR16 *);
            if (!s)
                s = (CHAR16 *)L"(null)";
            while (*s)
                emit(&o, end, *s++);
            fmt++;
        } else if (*fmt == 'c') {
            emit(&o, end, (CHAR16)__builtin_va_arg(ap, int));
            fmt++;
        } else if (*fmt == '%') {
            emit(&o, end, '%');
            fmt++;
        } else {
            emit(&o, end, '%');
            if (*fmt)
                emit(&o, end, *fmt++);
        }
    }
    __builtin_va_end(ap);
    *o = 0;
    len = (UINTN)(o - buf);

    if (ST && ST->ConOut)
        ST->ConOut->OutputString(ST->ConOut, buf);
    return len;
}
