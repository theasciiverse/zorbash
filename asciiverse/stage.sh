#!/bin/sh
# Lays out the av archive in stage/: the one binary (every picture, sound
# and tune is built into it) with the licence, readme and change notes.
set -eu
rm -rf stage && mkdir -p stage
cp zorbash stage/zorbash
cp src/LICENSE stage/LICENSE
cp README.md PATCHES.md UPSTREAM.md stage/
