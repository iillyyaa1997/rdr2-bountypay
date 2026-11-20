#!/bin/bash
# Cross-compile BountyPay.asi (x64 DLL) in a throwaway mingw-w64 container.
# usage: ./build.sh            -> out/BountyPay.asi
#        TEST=1 ./build.sh     -> out/BountyPay-test.asi (test hooks: commands from BountyPay.test)
set -euo pipefail
cd "$(dirname "$0")"
name=BountyPay; defs=""
if [ "${TEST:-0}" = 1 ]; then name=BountyPay-test; defs="-DBOUNTYPAY_TEST"; fi
docker build -q -t rdr2-asi-build . >/dev/null
mkdir -p out
docker run --rm -v "$PWD":/src -w /src rdr2-asi-build \
  x86_64-w64-mingw32-g++-posix -std=c++17 -O2 -Wall -Wextra -Wno-cast-function-type $defs \
  -shared -static -static-libgcc -static-libstdc++ -s \
  -o out/$name.asi src/bountypay.cpp
ls -l out/$name.asi
