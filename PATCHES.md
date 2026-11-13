# Changes from upstream

This is the Asciiverse work on [Zorbash](https://github.com/goblinhack/zorbash)
by goblinhack (Neil McGill), GPL-3.0 (`src/LICENSE`). The `asciiverse` branch
starts from upstream `4f69144` (9 November 2026) as a single commit, since the
upstream history (4.4 GB) is too large to push here; the full history is the
`upstream` remote.

## Ours

- **Terminal mode** (`--terminal`). The game's ascii mode already lays out
  everything (menus, map, sidebars, popups) as a grid of cells, which it then
  draws with OpenGL in a window. Terminal mode writes those same cells to the
  terminal it was started from instead, and takes keys and mouse from it. It
  is a separate backend, not a change to the game: everything else runs as
  before.
  - `src/terminal.cpp`, `src/my_terminal.hpp`: the backend. Output writes only
    the cells that changed, in 24-bit colour, repositioning the cursor after
    any non-ASCII character so terminals that measure a glyph differently
    cannot shift the screen. Input reads the kitty keyboard protocol where the
    terminal has it (real key releases), else plain bytes and the usual escape
    sequences, where a key counts as held for 90 ms after each press or auto
    repeat. Mouse is SGR reporting with motion (hover works). All of it is
    pushed into SDL's own event queue. Ctrl-C quits.
  - SDL keeps running for audio; video uses SDL's `offscreen` driver (set only
    if `SDL_VIDEODRIVER` is not already set), so no window opens.
  - The ascii grid takes the terminal's size (up to the game's 132x90 cap,
    centred when the terminal is larger). The UI lays itself out for it:
    100x30 plays well, 80x24 starts but squeezes the map.
  - While the game owns the screen, stdout and stderr go to the game's own log
    files (`appdata/zorbash/stdout.txt`, `stderr.txt`), since level creation
    prints to stdout. The terminal is restored on exit, on Ctrl-C and from the
    crash handler.
  - `src/ascii_dump.cpp`: the cell-to-text logic of `ascii_dump_to_console`
    moved into `ascii_cell_resolve`, which both the console dump and terminal
    mode use. The dump's output is unchanged.
  - `sdl_keyboard_state()` and `sdl_mouse_state()` (`src/sdl.cpp`) stand in for
    `SDL_GetKeyboardState` and `SDL_GetMouseState` at their six call sites, and
    answer from the terminal in terminal mode.
  - Hooks: option parsing and start-up (`src/main.cpp`), terminal restore in
    the signal handlers (`src/signal.cpp`), input polling (`src/sdl_loop.cpp`),
    no GL drawing in terminal mode (`src/ascii.cpp`, `src/sdl_display.cpp`),
    grid size from the terminal (`src/sdl.cpp`).

## Building

As upstream: `./RUNME`. For an incremental build afterwards, use
`USE_PRECOMPILED=yep make -f build/Makefile -j$(nproc) all`; a plain `make` in
`src/` fails upstream as well, since some sources rely on the precompiled
header for `<cmath>` and `<mutex>`.
