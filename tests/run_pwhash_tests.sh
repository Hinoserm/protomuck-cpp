#!/bin/sh
# Salted password hashing round-trip. Links tests/pwhash_selftest.cpp
# against the real PasswordHash/stringutil/sha1 objects from the build,
# so it exercises the shipping code rather than a copy.
#
# Usage: run_pwhash_tests.sh [build-dir]   (default: ../build)
set -e

here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/.." && pwd)
build=${1:-$root/build}
objs=$build/CMakeFiles/protomuck.dir/src

for o in PasswordHash stringutil sha1 random; do
    if [ ! -f "$objs/$o.cpp.o" ]; then
        echo "FAIL: $objs/$o.cpp.o missing; build the server first"
        exit 1
    fi
done

bin=$(mktemp -u /tmp/pwhash_selftest.XXXXXX)
g++ -std=c++17 -I "$root/src/inc" -o "$bin" \
    "$here/pwhash_selftest.cpp" \
    "$objs/PasswordHash.cpp.o" "$objs/stringutil.cpp.o" \
    "$objs/sha1.cpp.o" "$objs/random.cpp.o" \
    -lcrypto

"$bin"
rc=$?
rm -f "$bin"
exit $rc
