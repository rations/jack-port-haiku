#!/bin/sh
# make-hpkg.sh — build the `jack` .hpkg on a running Haiku (x86_64). The
# package pins no hrev, so build it on the nightly it will be installed on.
#
# Run ON Haiku (native). Produces jack-<version>-<revision>-x86_64.hpkg here in
# packaging/. Install it on a tester machine with:  pkgman install ./jack-*.hpkg
set -e

HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/.." && pwd)
VERSION=1.9.23
REVISION=6
STAGE="$HERE/stage"

# Build prerequisites (idempotent). A minimal Haiku lacks mpc/mpfr (compiler) and
# the base develop headers.
pkgman install -y gcc make pkgconfig mpc mpfr haiku_devel || true

# Always build from scratch. waf builds incrementally, so without this the
# package would be assembled from whatever happens to be in build/ -- which is
# not necessarily the checked-out source. Sources arriving by rsync keep their
# origin mtimes, so a build tree can also look newer than sources that are in
# fact more recent. A release artifact must not depend on either.
rm -rf "$STAGE" build
cd "$ROOT"

# Configure with the PACKAGED prefix, then stage the install under $STAGE via
# waf's --destdir (files land at $STAGE/boot/system/...).
#
# --no-libdbus: waf turns D-Bus device reservation on by itself whenever dbus-1
# is installed on the build machine, which links jackd to libdbus-1.so.3. That
# feature reserves ALSA cards through a Linux session bus and does nothing on
# Haiku, and the package does not require dbus, so jackd would fail to start on
# a machine without it.
#
# CC/CXX: waf prefers clang over gcc whenever clang is installed (it is on any
# machine that has built Wine), and lld hangs linking the hmulti driver. Pin the
# system gcc so the package does not depend on what else is installed.
CC=gcc CXX=g++ python3 ./waf configure --prefix=/boot/system --no-libdbus
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
