#!/bin/sh
# devenv.sh -- rebuild the throwaway test environment.
#
# This sandbox has no root and wipes /tmp between sessions, so QEMU, OVMF,
# gnu-efi and the image tools get unpacked into a prefix by hand. Nothing
# here is needed to build PicoOS on a normal machine -- see the README for
# the two-line apt install. This is only for testing it in the sandbox.
#
#   sh tools/devenv.sh          # unpacks into /tmp/qroot
#   . tools/devenv-env.sh       # puts it on PATH
set -e

ROOT=${ROOT:-/tmp/qroot}
DL=$ROOT/.deb

PKGS="qemu-system-x86 qemu-system-common qemu-system-data seabios ipxe-qemu
      gnu-efi ovmf mtools dosfstools xorriso
      libisoburn1t64 libisofs6t64 libburn4t64 libjte2
      libglib2.0-0t64 libpixman-1-0 libslirp0 libfdt1 libpng16-16t64
      libjpeg62-turbo libsnappy1v5 libzstd1 liblzo2-2 libcurl3t64-gnutls
      libgnutls30t64 libnettle8t64 libhogweed6t64 libgmp10 libtasn1-6
      libidn2-0 libunistring5 libp11-kit0 libffi8 libssh2-1t64
      libpcre2-8-0 libmount1 libblkid1 libselinux1 libseccomp2
      libudev1 libcap-ng0 libnuma1 libepoxy0 libgbm1 libdrm2
      libwayland-client0 libxkbcommon0 libbrotli1 libnghttp2-14
      librtmp1 libpsl5t64 libldap2 libsasl2-2 libkrb5-3 libk5crypto3
      libcom-err2 libkrb5support0 libkeyutils1 libgssapi-krb5-2
      libcapstone5 libpmem1 librdmacm1t64 libibverbs1 libvdeplug2t64
      libaio1t64 liburing2 libndctl6 libdaxctl1 libnl-3-200
      libnl-route-3-200 libkmod2"

mkdir -p "$DL"
cd "$DL"

echo "downloading packages..."
for p in $PKGS; do
    [ -f "$p.ok" ] && continue
    if apt-get download "$p" >/dev/null 2>&1; then
        touch "$p.ok"
    else
        echo "  (skipped $p -- not in the archive, probably fine)"
        touch "$p.ok"
    fi
done

echo "unpacking..."
for d in *.deb; do
    [ -f "$d" ] || continue
    dpkg-deb -x "$d" "$ROOT" 2>/dev/null || true
done

# gnu-efi ships its headers under a versioned path on some releases
if [ ! -f "$ROOT/usr/include/efi/efi.h" ]; then
    h=$(find "$ROOT" -name efi.h 2>/dev/null | head -1)
    [ -n "$h" ] && echo "  headers at $(dirname "$h")"
fi

# Debian ships the seabios roms as symlinks into /usr/share/qemu, and
# dpkg-deb -x does not run the maintainer scripts that create them
cp -n "$ROOT"/usr/share/seabios/*.bin "$ROOT"/usr/share/qemu/ 2>/dev/null || true

cat > "$ROOT/env.sh" <<EOF
export PATH=$ROOT/usr/bin:$ROOT/usr/sbin:\$PATH
export LD_LIBRARY_PATH=$ROOT/usr/lib/x86_64-linux-gnu:$ROOT/usr/lib
export QEMU_LD=$ROOT/usr/share/qemu
export EFIINC=$ROOT/usr/include/efi
export EFILIB=$ROOT/usr/lib
EOF

echo "done. run:  . $ROOT/env.sh"
ls "$ROOT/usr/bin" 2>/dev/null | grep -c . | sed 's/^/  binaries: /'
