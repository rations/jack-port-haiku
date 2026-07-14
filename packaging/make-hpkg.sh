#!/bin/sh
# make-hpkg.sh — build the `jack` .hpkg on a running Haiku (hrev59846 x86_64).
#
# Run ON Haiku (native). Produces jack-<version>-<revision>-x86_64.hpkg here in
# packaging/. Install it on a tester machine with:  pkgman install ./jack-*.hpkg
set -e

HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/.." && pwd)
VERSION=1.9.23
REVISION=1
STAGE="$HERE/stage"

# Build prerequisites (idempotent). A minimal Haiku lacks mpc/mpfr (compiler) and
# the base develop headers.
pkgman install -y gcc make pkgconfig mpc mpfr haiku_devel || true

rm -rf "$STAGE"
cd "$ROOT"

# Configure with the PACKAGED prefix, then stage the install under $STAGE via
# waf's --destdir (files land at $STAGE/boot/system/...).
python3 ./waf configure --prefix=/boot/system
python3 ./waf build
python3 ./waf install --destdir="$STAGE"

# The package tree root is the staged /boot/system contents; the .PackageInfo must
# sit at the top of the archived directory.
PKGROOT="$STAGE/boot/system"
[ -d "$PKGROOT" ] || { echo "!! expected staged tree at $PKGROOT" >&2; exit 1; }
cp "$HERE/jack.PackageInfo" "$PKGROOT/.PackageInfo"

OUT="$HERE/jack-$VERSION-$REVISION-x86_64.hpkg"
package create -C "$PKGROOT" "$OUT"
echo ">> built $OUT"
