#!/bin/sh
# Lays out the av archive in stage/: the one binary (every picture, sound
# and tune is built into it) with the licence, readme and change notes.
# On Windows (MSYS2), zorbash.exe and the few MSYS2 DLLs it links (ldd
# lists them under /ucrt64/bin), beside it.
set -eu
rm -rf stage && mkdir -p stage
if [ -f zorbash.exe ]; then
  cp zorbash.exe stage/zorbash.exe
  for dll in $(ldd zorbash.exe | awk '$3 ~ /^\/ucrt64\// { print $3 }'); do
    cp "$dll" stage/
  done
else
  cp zorbash stage/zorbash
fi
cp src/LICENSE stage/LICENSE
cp README.md PATCHES.md UPSTREAM.md stage/
ls -l stage
