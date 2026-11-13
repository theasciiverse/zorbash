//
// Copyright goblinhack@gmail.com
//

//
// Terminal mode (--terminal): play the ascii UI in the terminal you started
// the game from. The ascii cells are written to the terminal as text with
// true colour, and the terminal's keys and mouse are turned into SDL events,
// so the rest of the game does not need to know. There is no window and no
// OpenGL: SDL runs only its event queue, timers and audio.
//

#pragma once
#ifndef _MY_TERMINAL_HPP_
#define _MY_TERMINAL_HPP_

#include <cstdint>

//
// Pixels per ascii cell of the notional window terminal mode lays the UI out
// in (nothing is drawn at this size; the UI just measures itself in pixels)
//
#define TERMINAL_CELL_PIX_WIDTH  12
#define TERMINAL_CELL_PIX_HEIGHT 24

//
// How often the UI is redrawn in terminal mode
//
#define TERMINAL_FRAME_MS 33

extern bool g_opt_terminal;

#include <string>

//
// The folder terminal mode keeps its saves, config and logs under (created
// if need be): $XDG_STATE_HOME, else ~/.local/state on Linux and
// ~/Library/Application Support on macOS. Empty if there is no home.
//
std::string terminal_state_dir(void);

//
// Called first thing: keeps a handle on the terminal, sends everything else
// the game prints away from it, and reads its size. Returns false if
// stdin/stdout is not a terminal.
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
// Sleeps until terminal input arrives or ms pass, whichever is first.
//
void terminal_wait(int ms);

//
// Writes the ascii cells that changed since the last call.
//
void terminal_display(void);

//
// True once after the terminal changed size, so the UI can lay itself out again.
//
bool terminal_take_layout_change(void);

//
// Terminal size in cells, as the ascii UI should use it.
//
int terminal_cols(void);
int terminal_rows(void);

//
// The config as loaded from disk, before terminal mode sizes things for
// itself; and the same window and graphics settings put back into a config
// that is about to be saved.
//
class Config_;
typedef class Config_ Config;
void terminal_remember_config(void);
void terminal_config_for_saving(Config &c, bool *ascii_mode);

//
// Stand-ins for SDL's state queries, which know nothing of the terminal.
//
const uint8_t *terminal_keyboard_state(void);
uint32_t       terminal_mouse_state(int *x, int *y);

#endif
