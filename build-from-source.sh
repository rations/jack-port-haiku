#!/bin/sh
# build-from-source.sh — build and install jack-port-haiku from source on Haiku.
#
# Run ON Haiku (native). Installs jackd, libjack/libjackserver, the backends and the
# example tools into the non-packaged tree (/boot/system/non-packaged), which is on
# the default runtime/search paths. This is the "I'd rather build it myself" path;
# for a prebuilt binary use packaging/make-hpkg.sh instead.
set -e

HERE=$(cd "$(dirname "$0")" && pwd)

# A minimal/nightly Haiku lacks these: mpc and mpfr are needed by the compiler,
# pkgconfig by the build, and haiku_devel provides the base develop headers.
pkgman install -y gcc make pkgconfig mpc mpfr haiku_devel

cd "$HERE"
python3 ./waf configure --prefix=/boot/system/non-packaged
python3 ./waf build
python3 ./waf install

echo ">> Installed jackd + libjack under /boot/system/non-packaged."
echo ">> Try: jackd -d hmulti -d /dev/audio/hmulti/usb/1 -r 48000 -p 128 -n 3"
