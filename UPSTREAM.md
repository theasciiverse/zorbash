# Terminal mode, for upstream

Hi Neil. This branch adds a terminal mode to Zorbash: `zorbash --terminal`
plays the ascii UI in the terminal it was started from, with no window and no
OpenGL. It is written to be easy to take upstream, or to leave out: one flag,
one backend file and small guarded hooks. With the flag off, the game runs
exactly as before.

## The idea

Ascii mode already does the hard part. Every screen (map, side bars, menus,
popups) is laid out as the `cells` grid in `ascii.cpp`, and
`ascii_dump_to_console` already knew how to turn a cell into a character and
two colours. Terminal mode writes that grid to the terminal instead of
drawing it with GL, and feeds the terminal's keys and mouse to the game as
ordinary SDL events. Game logic, widgets and the ascii map are untouched.

## Files

- `src/terminal.cpp`, `src/my_terminal.hpp`: the backend.
  - **Output**: each frame, the cells that changed are written with 24-bit
    colour escapes, about 30 frames a second. After any non-ASCII character
    the cursor is placed again, because terminals disagree on some glyph
    widths.
  - **Keys**: the kitty keyboard protocol where the terminal has it (Ghostty,
    kitty, WezTerm, foot), which gives real key releases, so holding a key to
    walk works as in the window. Elsewhere, plain bytes and the usual escape
    sequences, with a key counted as held for 90 ms after each press or auto
    repeat. Keys become `SDL_KEYDOWN`/`SDL_KEYUP` events, and
    `sdl_keyboard_state()` answers from the same state.
  - **Mouse**: SGR mouse reporting with motion, so hover and clicks work,
    turned into SDL mouse events at the centre of the cell.
  - **Screen**: alternate screen, raw mode, restored on every exit, including
    from the crash handler. Ctrl-C quits.
- `src/ascii_dump.cpp`: the cell-to-text part of `ascii_dump_to_console`
  moved into `ascii_cell_resolve()`, shared by both. The dump prints the same
  as before.

## Hooks (all behind `g_opt_terminal`)

- `main.cpp`: `--terminal` (implies `--ascii`), looked for before the log
  files open; `terminal_init()` first thing, `terminal_start()` before the
  main loop. With `APPDATA` unset, terminal mode keeps its folder in
  `$XDG_STATE_HOME/zorbash` (else `~/.local/state`, or `~/Library/Application
  Support` on macOS), since it is often started from somewhere else. A build
  with `-DZORBASH_TERMINAL_ONLY` has no other mode.
- `sdl.cpp`: `sdl_init()` starts only SDL's events, timers and audio, with no
  video and no GL context. The ascii grid takes the terminal's size (up to
  `TERM_WIDTH_MAX` by `TERM_HEIGHT_MAX`). `sdl_keyboard_state()` and
  `sdl_mouse_state()` stand in for `SDL_GetKeyboardState` and
  `SDL_GetMouseState` at their six call sites.
- `gl.cpp`, `my_gl.hpp`, `tex.cpp`: `GL_TERMINAL_SKIP()` at the top of the
  GL helpers, `GL_ERROR_CHECK` off, no texture upload. The few direct GL
  calls on the ascii path are guarded too (`wid.cpp`, `thing_level_enter.cpp`,
  `sdl_loop.cpp`, `sdl.cpp`). I ran it under gdb with a breakpoint on every
  GL entry point in libGL, through menus, a new game, play, inventory, help,
  save and load: none was hit.
- `sdl_loop.cpp`: polls the terminal; redraws at about 30 frames a second;
  lays the UI out again (`sdl_display_reset()`) when the terminal is
  resized.
- `sdl_display.cpp`, `ascii.cpp`: nothing drawn with GL; `ascii_display()`
  hands the grid to the terminal.
- `game_save.cpp`: the config saved from terminal mode keeps the window
  settings and graphics mode as they were loaded, so the next windowed start
  is unchanged.
- `wid_main_menu.cpp`, `wid_topcon.cpp`, `wid_config.cpp`: no Tab to pixel
  art, and no Graphics page, in terminal mode.
- `signal.cpp`: the crash and Ctrl-C handlers restore the terminal first.

## Two small fixes that apply without terminal mode

- `log.cpp`: `CROAK_CLEAN` went through `die()`, which exits 1, so a normal
  Quit Game exited 1. It now tidies up and exits 0. Closing the window
  (`SDL_QUIT`) uses `DIE_CLEAN` too.
- `wid_actionbar_ascii.cpp`: on screens too narrow for the action bar
  between the side bars (below about 130 columns), the buttons narrow so
  "Quit" is not hidden under the left bar.

- `level_scroll.cpp`: the ascii map was pinned at (0,0), which shows a
  whole level only when the screen is big enough (a level is 96x48; the map
  area at 132x46 is about 90x40). It now scrolls when the level does not
  fit, clamped to the level, and stays pinned when it does. The test for
  scrolling near an edge used the player's last drawn position against the
  whole screen; it now uses the player's map position against the map area,
  so the player cannot walk under the right-hand bar (which could happen at
  132x46 too, between columns 110 and 124).

- `wid.cpp`: Robo mode stopped for good at the level map after taking the
  stairs: `wid_choose_next_dungeons_tick` sets `game->request_to_choose_level`,
  but nothing ever read it (since the first commit). `wid_handle_requests`
  now enters the chosen level.

- `level_tick.cpp`, `main.cpp`, `globals.cpp`: `--robot-delay <ms>`, an
  option of its own: in Robo mode, `Level::tick()` runs at most once per
  delay, so the robot can be watched at a person's pace (we record the
  catalog preview with 50 ms, about four steps a second). Off by default.

## Sizes

100x30 and up plays well; 80x24 starts, but the map is a sliver.

## Building without GL or a GPU

`asciiverse/build.sh` is how we build it for the Asciiverse catalog: SDL2 and
SDL2_mixer from source without video, linked statically, your `RUNME --rel`
on top, and on Linux a `libGL.a` of empty functions
(`asciiverse/glstubs.sh`), since terminal mode never calls GL. The result is
one binary that needs no GPU, display server or GL library.

Happy to send it as a pull request, in whatever shape suits you.
