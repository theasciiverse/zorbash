//
// Copyright goblinhack@gmail.com
//

#include "my_array_bounds_check.hpp"
#include "my_ascii.hpp"
#include "my_color_defs.hpp"
#include "my_console.hpp"
#include "my_font.hpp"
#include "my_game.hpp"
#include "my_sdl_event.hpp"
#include "my_string.hpp"
#include "my_ui.hpp"
#include "my_unicode.hpp"
#include "my_wid.hpp"

//
// What one ascii cell looks like as text: its character and colours.
// Shared by the console dump and terminal mode.
//
void ascii_cell_resolve(int x, int y, wchar_t *out_ch, color *out_fg, color *out_bg)
{
  const AsciiCell *cell = &getref_no_check(cells, x, y);

  color bg = COLOR_NONE;
  color fg = COLOR_NONE;

  //
  // Get the background colors
  //
  for (int depth = TILE_LAYER_BG_0; depth < TILE_LAYER_FG_0; depth++) {
    if (cell->tex[ depth ]) {
      continue;
    }
    if (! cell->tile[ depth ]) {
      continue;
    }
    auto col = cell->color_tl[ depth ];
    if (col != COLOR_NONE) {
      bg = col;
    }
  }

  //
  // Get the foreground colors
  //
  for (int depth = TILE_LAYER_FG_0; depth < TILE_LAYER_MAX; depth++) {
    if (cell->tex[ depth ]) {
      continue;
    }
    if (! cell->tile[ depth ]) {
      continue;
    }
    auto col = cell->color_tl[ depth ];
    if (col != COLOR_NONE) {
      fg = col;
    }
  }

  //
  // Avoid white on white
  //
  if ((bg == WHITE) && (fg == WHITE)) {
    bg = GRAY50;
  }

  //
  // Fake the window backgrounds that are just big textures
  //
  for (int depth = TILE_LAYER_BG_0; depth < TILE_LAYER_MAX; depth++) {
    if (cell->tile[ depth ]) {
      continue;
    }

    if ((cell->tx[ depth ] != 0) || (cell->ty[ depth ] != 0) || (cell->dx[ depth ] != 0)
        || (cell->dy[ depth ] != 0)) {
      bg = GRAY10;
      bg.r += depth * 20;
      bg.g += depth * 20;
      bg.b += depth * 20;
      break;
    }
  }

  //
  // Get the unicode char to print
  //
  wchar_t ch = ' ';
  for (int depth = TILE_LAYER_MAX - 1; depth >= 0; depth--) {
    if (! cell->tile[ depth ]) {
      continue;
    }
    if (cell->tex[ depth ]) {
      continue;
    }
    if (cell->ch[ depth ]) {
      ch = unicode_alias_to_char(cell->ch[ depth ]);
      break;
    }
  }

  *out_ch = ch;
  *out_fg = fg;
  *out_bg = bg;
}

void ascii_dump_to_console(FILE *out, bool use_color)
{
  TRACE_NO_INDENT();

  char fgstr[ 200 ];
  int  x;
  int  y;

  fprintf(out, "%lc", '+');
  for (x = 0; x < TERM_WIDTH; x++) {
    fprintf(out, "%lc", '-');
  }
  fprintf(out, "%lc", '+');
  fprintf(out, "%lc", '\n');

  for (y = 0; y < TERM_HEIGHT; y++) {
    fprintf(out, "%lc", '|');
    for (x = 0; x < TERM_WIDTH; x++) {
      wchar_t ch;
      color   fg;
      color   bg;
      ascii_cell_resolve(x, y, &ch, &fg, &bg);

      if (use_color) {
        if ((fg != COLOR_NONE) && (bg != COLOR_NONE)) {
          snprintf(fgstr, SIZEOF(fgstr), "\e[38;2;%u;%u;%u;48;2;%u;%u;%um", fg.r, fg.g, fg.b, bg.r, bg.g, bg.b);
        } else if (fg != COLOR_NONE) {
          snprintf(fgstr, SIZEOF(fgstr), "\e[38;2;%u;%u;%um", fg.r, fg.g, fg.b);
        } else if (bg != COLOR_NONE) {
          snprintf(fgstr, SIZEOF(fgstr), "\e[48;2;%u;%u;%um", bg.r, bg.g, bg.b);
        } else {
          snprintf(fgstr, SIZEOF(fgstr), "\e[39m\e[49m");
        }
        fprintf(out, "%s", fgstr);
      }

      fprintf(out, "%lc", ch);

      //
      // Reset colors
      //
      if (use_color) {
        snprintf(fgstr, SIZEOF(fgstr), "\e[39m\e[49m");
        fprintf(out, "%s", fgstr);
      }
    }

    fprintf(out, "%lc", '|');
    fprintf(out, "%lc", '\n');
  }

  fprintf(out, "%lc", '+');
  for (x = 0; x < TERM_WIDTH; x++) {
    fprintf(out, "%lc", '-');
  }
  fprintf(out, "%lc", '+');
  fprintf(out, "%lc", '\n');
}
