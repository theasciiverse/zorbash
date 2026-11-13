# Changes from upstream

This is the Asciiverse edition of [Zorbash](https://github.com/goblinhack/zorbash)
by Neil McGill (goblinhack), GPL-3.0 (`src/LICENSE`). The `asciiverse` branch
starts from upstream `4f69144` (9 November 2026, after its `v0.2.6` tag) as a
single commit, since the upstream history (4.4 GB) is too large to push here;
the full history is the `upstream` remote. `UPSTREAM.md` explains the terminal
mode for Neil, file by file.

## Ours

- **Terminal mode** (`--terminal`): the ascii UI played in the terminal the
  game was started from, with no window and no OpenGL.
  - `src/terminal.cpp`, `src/my_terminal.hpp`: the backend. It writes the
    ascii cells that changed, in 24-bit colour, about 30 times a second. Keys
    come from the kitty keyboard protocol where the terminal has it (real key
    releases), else from plain bytes and escape sequences, with a key held
    for 90 ms after each press or auto repeat. The mouse comes from SGR
    reporting with motion, so hover works. Both go to the game as SDL
    events. Ctrl-C quits.
  - SDL runs only its events, timers and audio: no video, no GL context. The
    GL helpers, texture upload and the few direct GL calls on the ascii path
    return at once in terminal mode.
  - The grid takes the terminal's size and lays itself out again on a
    resize. 100x30 and up plays well.
  - Saves, config and logs go to `$XDG_STATE_HOME/zorbash` (else
    `~/.local/state/zorbash`, or `~/Library/Application Support/zorbash` on
    macOS) unless `APPDATA` is set. The config saved from terminal mode keeps
    the player's window settings and graphics mode as they were.
  - The terminal shows only the game: anything else printed goes to the log
    files. It is restored on every exit, including from the crash handler.
  - No Tab to pixel art and no Graphics options page in terminal mode, and
    terminal mode is always ascii, whatever the config says.
  - `ascii_dump_to_console`'s cell-to-text logic is now `ascii_cell_resolve`,
    shared with terminal mode; the dump prints the same.
  - `sdl_keyboard_state()` and `sdl_mouse_state()` stand in for SDL's state
    queries at their six call sites.
  - `-DZORBASH_TERMINAL_ONLY` builds a game whose only mode is the terminal.
- **The ascii map scrolls on smaller screens.** It was pinned at the level's
  top left, which shows a whole level (96x48) only at about 132x46 and up. It
  now scrolls when the level does not fit, clamped to the level, and keeps
  the player away from the map area's edges (`src/level_scroll.cpp`).
- **The action bar fits between the side bars** on narrower screens, its
  buttons narrowing as needed, so Quit is no longer under the left bar
  (`src/wid_actionbar_ascii.cpp`).
- **Robo mode goes on past the stairs.** The robot's pick on the level map
  (`request_to_choose_level`) was never acted on, so it stopped there for
  good; `wid_handle_requests` now enters the chosen level (`src/wid.cpp`).
- **A clean quit exits 0.** `CROAK_CLEAN` went through `die()` and exit 1;
  closing the window or Ctrl-C now quits cleanly too (`src/log.cpp`,
  `src/sdl_events.cpp`).

## Building

As upstream: `./RUNME`. For an incremental build afterwards, use
`USE_PRECOMPILED=yep make -f build/Makefile -j$(nproc) all`; a plain `make` in
`src/` fails upstream as well, since some sources rely on the precompiled
header for `<cmath>` and `<mutex>`.

## Packaging

- `asciiverse/build.sh`: the release build, terminal-only. SDL2 and
  SDL2_mixer built from source without video and linked statically, then
  upstream's `RUNME --rel`; on Linux, `asciiverse/glstubs.sh` writes a
  `libGL.a` of empty functions to link in place of the system's, since
  terminal mode never calls OpenGL. The binary needs no GPU, display server,
  SDL or GL library, and carries every picture, sound and tune inside it.
- `asciiverse/stage.sh`, `asciiverse/nfpm.yaml`,
  `.github/workflows/release.yml`: av archives for Linux (x86-64 and ARM64,
  glibc 2.35 and newer) and macOS (x86-64 and ARM64), and .deb, .rpm and
  Arch packages. No Windows build: the terminal backend reads the terminal
  through POSIX (termios, poll).
- `asciiverse/record-preview.py`, `asciiverse/zorbash.cast`: the catalog
  preview, 100x30: the title screen, the dungeon's map, then the game's own
  robot playing the first level.
