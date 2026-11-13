#!/bin/sh
# Builds Zorbash for the Asciiverse catalog as ./zorbash: the terminal mode
# only (--terminal is the default and the only mode), with every picture,
# sound and tune built into the one binary.
#
# The build is upstream's own (./RUNME --rel), pointed at:
#   - SDL2 and SDL2_mixer built from source into avbuild/ and linked in
#     statically, SDL2 without video (the game draws in the terminal and uses
#     SDL for its events, timers and sound), SDL2_mixer with its own Ogg
#     Vorbis decoder (the music is Ogg, the sounds WAV). SDL still loads the
#     system's sound library (PipeWire, PulseAudio or ALSA) when the game
#     starts, so Linux builds are glibc: run inside ubuntu:22.04 for glibc
#     2.35 and newer,
#       docker run --rm -v "$PWD":/src -w /src ubuntu:22.04 sh asciiverse/build.sh
#   - on Linux, a libGL.a of empty functions (asciiverse/glstubs.sh), so the
#     binary needs no OpenGL library: terminal mode never calls one. macOS
#     links the system's OpenGL framework, present on every Mac and likewise
#     never called. Run on the runner with CC_ARCH set to x86_64 or arm64.
# A small sdl2-config (avbuild/bin) tells RUNME where SDL is and adds
# -DZORBASH_TERMINAL_ONLY.
set -eu
SDL2=2.32.10
SDL2_MIXER=2.8.1

if [ -f /etc/os-release ] && grep -q '^ID=ubuntu' /etc/os-release && [ "$(id -u)" = 0 ]; then
  export DEBIAN_FRONTEND=noninteractive
  apt-get update -qq
  # libgl-dev for the OpenGL headers only: the binary links asciiverse/glstubs.sh
  apt-get install -y -qq build-essential clang cmake git pkg-config python3 file libgl-dev \
    libasound2-dev libpulse-dev libpipewire-0.3-dev >/dev/null
fi

root=$(pwd)
# no "-" in this path: RUNME reads the include folder out of
# sdl2-config with a pattern that has no "-" in it
deps="$root/avbuild"
prefix="$deps/prefix"
mkdir -p "$deps/bin" "$deps/lib"
arch=${CC_ARCH:+-DCMAKE_OSX_ARCHITECTURES=$CC_ARCH}
jobs=${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)}

if [ ! -f "$prefix/lib/libSDL2.a" ]; then
  [ -d "$deps/SDL" ] || git clone -q --depth 1 -b "release-$SDL2" https://github.com/libsdl-org/SDL "$deps/SDL"
  cmake -S "$deps/SDL" -B "$deps/SDL-build" -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$prefix" -DSDL_SHARED=OFF -DSDL_STATIC=ON \
    -DSDL_TEST=OFF -DSDL_VIDEO=OFF -DSDL_RENDER=OFF -DSDL_JOYSTICK=OFF \
    -DSDL_HAPTIC=OFF -DSDL_HIDAPI=OFF -DSDL_SENSOR=OFF -DSDL_DLOPEN=ON \
    -DSDL_STATIC_PIC=ON $arch >/dev/null
  cmake --build "$deps/SDL-build" -j "$jobs" >/dev/null
  cmake --install "$deps/SDL-build" >/dev/null
fi

if [ ! -f "$prefix/lib/libSDL2_mixer.a" ]; then
  [ -d "$deps/SDL_mixer" ] || git clone -q --depth 1 -b "release-$SDL2_MIXER" https://github.com/libsdl-org/SDL_mixer "$deps/SDL_mixer"
  cmake -S "$deps/SDL_mixer" -B "$deps/SDL_mixer-build" -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$prefix" -DCMAKE_PREFIX_PATH="$prefix" \
    -DBUILD_SHARED_LIBS=OFF -DSDL2MIXER_SAMPLES=OFF -DSDL2MIXER_VORBIS=STB \
    -DSDL2MIXER_OPUS=OFF -DSDL2MIXER_FLAC=OFF -DSDL2MIXER_MOD=OFF \
    -DSDL2MIXER_MIDI=OFF -DSDL2MIXER_MP3=OFF -DSDL2MIXER_WAVPACK=OFF \
    -DSDL2MIXER_GME=OFF $arch >/dev/null
  cmake --build "$deps/SDL_mixer-build" -j "$jobs" >/dev/null
  cmake --install "$deps/SDL_mixer-build" >/dev/null
fi

export PKG_CONFIG_PATH="$prefix/lib/pkgconfig"
sdl_cflags=$(pkg-config --cflags sdl2 | sed 's/-D_REENTRANT//')
sdl_libs=$(pkg-config --static --libs SDL2_mixer sdl2)
glstub=""
if [ "$(uname)" != Darwin ]; then
  sh "$root/asciiverse/glstubs.sh" "$root/src" "$deps/lib"
  glstub="-L$deps/lib"
fi

# RUNME asks sdl2-config for flags, and looks for SDL_mixer.h beside SDL.h
cat > "$deps/bin/sdl2-config" <<EOF
#!/bin/sh
case "\$1" in
  --cflags) echo "$sdl_cflags -DZORBASH_TERMINAL_ONLY${CC_ARCH:+ -arch $CC_ARCH}" ;;
  --libs) echo "$glstub $sdl_libs${CC_ARCH:+ -arch $CC_ARCH}" ;;
  *) exit 1 ;;
esac
EOF
chmod +x "$deps/bin/sdl2-config"

rm -f "$root/zorbash"
PATH="$deps/bin:$PATH" ./RUNME --rel --github
file zorbash
strip zorbash
ls -l zorbash
