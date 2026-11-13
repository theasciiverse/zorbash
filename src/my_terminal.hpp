//
// Copyright goblinhack@gmail.com
//

//
// Terminal mode (--terminal): play the ascii UI in the terminal you started
// the game from. The ascii cells are written to the terminal as text with
// true colour, and the terminal's keys and mouse are turned into SDL events.
// SDL still runs (audio, and video on its offscreen driver), so the rest of
// the game does not need to know.
//

#pragma once
#ifndef _MY_TERMINAL_HPP_
#define _MY_TERMINAL_HPP_

#include <cstdint>

//
// Pixels per ascii cell for the (unseen) offscreen window in terminal mode
//
#define TERMINAL_CELL_PIX_WIDTH  12
#define TERMINAL_CELL_PIX_HEIGHT 24

extern bool g_opt_terminal;

//
// Called before sdl_init: picks the offscreen SDL video driver and reads the
// terminal size. Returns false if stdin/stdout is not a terminal.
//
bool terminal_init(void);

//
// Puts the terminal into raw mode, alternate screen, mouse and key reporting.
//
void terminal_start(void);

//
// Restores the terminal. Safe to call more than once, and from a signal handler.
//
void terminal_fini(void);

//
// Reads pending terminal input and pushes it as SDL events.
//
void terminal_poll(void);

//
// Writes the ascii cells that changed since the last call.
//
void terminal_display(void);

//
// Terminal size in cells, as the ascii UI should use it.
//
int terminal_cols(void);
int terminal_rows(void);

//
// Stand-ins for SDL's state queries, which know nothing of the terminal.
//
const uint8_t *terminal_keyboard_state(void);
uint32_t       terminal_mouse_state(int *x, int *y);

#endif
