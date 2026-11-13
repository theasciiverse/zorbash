//
// Copyright goblinhack@gmail.com
//

//
// Terminal mode: see my_terminal.hpp.
//
// Output: each frame, the ascii cells (the same ones the OpenGL ascii mode
// draws) are resolved to a character and two colours, compared with what is
// already on the terminal, and only the changed cells are written, with
// 24-bit colour escapes.
//
// Input: keys come from the kitty keyboard protocol where the terminal has
// it (press, repeat and release events), else from plain bytes and the usual
// escape sequences, where a key counts as held for a short while after each
// press or auto repeat. Mouse comes from SGR mouse reporting with motion.
// Both are pushed into SDL's queue as ordinary SDL events, and SDL's keyboard
// and mouse state queries are answered from here.
//

#include "my_array_bounds_check.hpp"
#include "my_ascii.hpp"
#include "my_color_defs.hpp"
#include "my_game.hpp"
#include "my_globals.hpp"
#include "my_globals_extra.hpp"
#include "my_sdl_event.hpp"
#include "my_sdl_proto.hpp"
#include "my_terminal.hpp"

#include <SDL.h>
#include <clocale>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <fcntl.h>
#include <string>
#include <sys/ioctl.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

bool g_opt_terminal;

static bool            term_started;
static int             term_fd = -1;
static int             term_saved_stderr = -1;
static bool            term_have_saved_termios;
static struct termios  term_saved_termios;
static int             term_cols = 80;
static int             term_rows = 24;
static volatile bool   term_resized;
static bool            term_kitty_keys;
static uint8_t         term_keys[ SDL_NUM_SCANCODES ];
static uint64_t        term_key_release_at[ SDL_NUM_SCANCODES ];
static SDL_Keysym      term_key_sym[ SDL_NUM_SCANCODES ];
static int             term_mouse_cx = -1;
static int             term_mouse_cy = -1;
static uint32_t        term_mouse_buttons;
static std::string     term_in;
static uint64_t        term_in_esc_at;
static uint64_t        term_last_frame_ms;

//
// What is on the terminal now
//
struct TermCell {
  wchar_t ch;
  color   fg;
  color   bg;
};
static TermCell term_screen[ TERM_HEIGHT_MAX ][ TERM_WIDTH_MAX ];
static bool     term_screen_valid;
static int      term_off_x;
static int      term_off_y;

//
// How long a key counts as held, without the kitty protocol, after a press
// or an auto repeat. Long enough to bridge auto repeat (about 30 ms apart),
// short enough that a tap moves once.
//
static const uint64_t TERM_KEY_HOLD_MS = 90;

//
// Wait this long for the rest of an escape sequence before taking a lone ESC
// as the Escape key.
//
static const uint64_t TERM_ESC_WAIT_MS = 30;

static uint64_t term_now_ms(void)
{
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t) ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void term_write(const std::string &s)
{
  const char *p = s.data();
  size_t      n = s.size();
  while (n) {
    ssize_t w = write(term_fd, p, n);
    if (w < 0) {
      if (errno == EINTR || errno == EAGAIN) {
        continue;
      }
      return;
    }
    p += w;
    n -= (size_t) w;
  }
}

static void term_read_size(void)
{
  struct winsize ws;
  if (ioctl(term_fd, TIOCGWINSZ, &ws) == 0 && ws.ws_col && ws.ws_row) {
    term_cols = ws.ws_col;
    term_rows = ws.ws_row;
  }
}

static void term_on_winch(int) { term_resized = true; }

int terminal_cols(void) { return std::min(term_cols, (int) TERM_WIDTH_MAX); }
int terminal_rows(void) { return std::min(term_rows, (int) TERM_HEIGHT_MAX); }

bool terminal_init(void)
{
  if (! isatty(STDIN_FILENO) || ! isatty(STDOUT_FILENO)) {
    return false;
  }

  //
  // No window: SDL renders (and we ignore) offscreen. Leave it alone if the
  // player picked a driver themselves.
  //
  setenv("SDL_VIDEODRIVER", "offscreen", 0);

  //
  // For wcwidth on the characters we write
  //
  setlocale(LC_CTYPE, "");

  //
  // Our own handle on the terminal, so that anything else the game prints
  // can be sent elsewhere once we take over the screen.
  //
  term_fd = dup(STDOUT_FILENO);

  term_read_size();
  return true;
}

void terminal_start(void)
{
  if (term_started) {
    return;
  }

  if (tcgetattr(STDIN_FILENO, &term_saved_termios) == 0) {
    term_have_saved_termios = true;
    struct termios raw      = term_saved_termios;
    raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    raw.c_oflag &= ~(OPOST);
    raw.c_cflag |= (CS8);
    raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
    raw.c_cc[ VMIN ]  = 0;
    raw.c_cc[ VTIME ] = 0;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
  }

  int flags = fcntl(STDIN_FILENO, F_GETFL, 0);
  fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK);

  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));
  sa.sa_handler = term_on_winch;
  sigaction(SIGWINCH, &sa, nullptr);

  //
  // From here on the screen is ours: stray prints from the game go to its log files.
  //
  term_saved_stderr = dup(STDERR_FILENO);
  if (g_log_stdout) {
    fflush(stdout);
    dup2(fileno(g_log_stdout), STDOUT_FILENO);
  }
  if (g_log_stderr) {
    fflush(stderr);
    dup2(fileno(g_log_stderr), STDERR_FILENO);
  }

  term_started = true;
  atexit(terminal_fini);

  //
  // Alternate screen, hide cursor, no autowrap, mouse with motion in SGR form,
  // then push kitty keyboard flags (disambiguate, event types, all keys as
  // escapes) and ask whether they took: a terminal with the protocol answers
  // CSI ? flags u before its CSI c reply.
  //
  term_write("\x1b[?1049h\x1b[?25l\x1b[?7l\x1b[2J"
             "\x1b[?1003h\x1b[?1006h"
             "\x1b[>11u\x1b[?u\x1b[c");

  term_screen_valid = false;
}

void terminal_fini(void)
{
  if (! term_started) {
    return;
  }
  term_started = false;

  //
  // Only async-signal-safe calls from here on, as this runs from the crash handler too
  //
  static const char reset[] = "\x1b[<u\x1b[?1003l\x1b[?1006l\x1b[0m\x1b[?7h\x1b[?25h\x1b[?1049l";
  ssize_t           unused  = write(term_fd, reset, sizeof(reset) - 1);
  (void) unused;

  //
  // Give stdout and stderr back to the terminal for any last words
  //
  dup2(term_fd, STDOUT_FILENO);
  if (term_saved_stderr >= 0) {
    dup2(term_saved_stderr, STDERR_FILENO);
  }

  if (term_have_saved_termios) {
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &term_saved_termios);
  }

  int flags = fcntl(STDIN_FILENO, F_GETFL, 0);
  fcntl(STDIN_FILENO, F_SETFL, flags & ~O_NONBLOCK);
}

const uint8_t *terminal_keyboard_state(void) { return term_keys; }

uint32_t terminal_mouse_state(int *x, int *y)
{
  if (term_mouse_cx < 0) {
    if (x) {
      *x = 0;
    }
    if (y) {
      *y = 0;
    }
    return term_mouse_buttons;
  }

  int gw = std::max(1, (int) game->config.ascii_gl_width);
  int gh = std::max(1, (int) game->config.ascii_gl_height);
  if (x) {
    *x = term_mouse_cx * gw + gw / 2;
  }
  if (y) {
    *y = term_mouse_cy * gh + gh / 2;
  }
  return term_mouse_buttons;
}

////////////////////////////////////////////////////////////////////////////
// Keys
////////////////////////////////////////////////////////////////////////////

//
// Kitty protocol functional key codes we care about
//
enum {
  KITTY_KP_0          = 57399,
  KITTY_KP_9          = 57408,
  KITTY_KP_DECIMAL    = 57409,
  KITTY_KP_DIVIDE     = 57410,
  KITTY_KP_MULTIPLY   = 57411,
  KITTY_KP_SUBTRACT   = 57412,
  KITTY_KP_ADD        = 57413,
  KITTY_KP_ENTER      = 57414,
  KITTY_KP_EQUAL      = 57415,
  KITTY_KP_LEFT       = 57417,
  KITTY_KP_RIGHT      = 57418,
  KITTY_KP_UP         = 57419,
  KITTY_KP_DOWN       = 57420,
  KITTY_KP_PAGE_UP    = 57421,
  KITTY_KP_PAGE_DOWN  = 57422,
  KITTY_KP_HOME       = 57423,
  KITTY_KP_END        = 57424,
  KITTY_KP_INSERT     = 57425,
  KITTY_KP_DELETE     = 57426,
  KITTY_KP_BEGIN      = 57427,
  KITTY_MODIFIER_LOW  = 57441,
  KITTY_MODIFIER_HIGH = 57452,
};

//
// Our own ids for keys that have no character
//
enum {
  TK_NONE = 0,
  TK_UP   = 0x110000,
  TK_DOWN,
  TK_LEFT,
  TK_RIGHT,
  TK_HOME,
  TK_END,
  TK_PAGEUP,
  TK_PAGEDOWN,
  TK_INSERT,
  TK_DELETE,
  TK_F1,
  TK_F12 = TK_F1 + 11,
};

static uint16_t term_mods_to_sdl(int m)
{
  uint16_t mod = KMOD_NONE;
  if (m & 1) {
    mod |= KMOD_LSHIFT;
  }
  if (m & 2) {
    mod |= KMOD_LALT;
  }
  if (m & 4) {
    mod |= KMOD_LCTRL;
  }
  if (m & 8) {
    mod |= KMOD_LGUI;
  }
  return mod;
}

//
// A key (a code point or one of ours) and modifiers, to SDL's terms.
// Returns false for keys we do not pass on.
//
static bool term_key_to_sdl(int key, uint16_t mod, SDL_Keysym *ks)
{
  memset(ks, 0, sizeof(*ks));
  SDL_Keycode sym = SDLK_UNKNOWN;
  SDL_Scancode sc = SDL_SCANCODE_UNKNOWN;

  if (key >= KITTY_MODIFIER_LOW && key <= KITTY_MODIFIER_HIGH) {
    return false;
  }

  switch (key) {
    case 13 : sym = SDLK_RETURN; break;
    case 27 : sym = SDLK_ESCAPE; break;
    case 9 : sym = SDLK_TAB; break;
    case 127 :
    case 8 : sym = SDLK_BACKSPACE; break;
    case 32 : sym = SDLK_SPACE; break;
    case TK_UP : sym = SDLK_UP; break;
    case TK_DOWN : sym = SDLK_DOWN; break;
    case TK_LEFT : sym = SDLK_LEFT; break;
    case TK_RIGHT : sym = SDLK_RIGHT; break;
    case TK_HOME : sym = SDLK_HOME; break;
    case TK_END : sym = SDLK_END; break;
    case TK_PAGEUP : sym = SDLK_PAGEUP; break;
    case TK_PAGEDOWN : sym = SDLK_PAGEDOWN; break;
    case TK_INSERT : sym = SDLK_INSERT; break;
    case TK_DELETE : sym = SDLK_DELETE; break;
    case KITTY_KP_DECIMAL :
    case KITTY_KP_DELETE : sym = SDLK_KP_PERIOD; break;
    case KITTY_KP_DIVIDE : sym = SDLK_KP_DIVIDE; break;
    case KITTY_KP_MULTIPLY : sym = SDLK_KP_MULTIPLY; break;
    case KITTY_KP_SUBTRACT : sym = SDLK_KP_MINUS; break;
    case KITTY_KP_ADD : sym = SDLK_KP_PLUS; break;
    case KITTY_KP_ENTER : sym = SDLK_KP_ENTER; break;
    case KITTY_KP_EQUAL : sym = SDLK_KP_EQUALS; break;
    //
    // Keypad with num lock off: the game reads the keypad as 7 8 9 / 4 6 / 1 2 3
    //
    case KITTY_KP_LEFT : sym = SDLK_KP_4; break;
    case KITTY_KP_RIGHT : sym = SDLK_KP_6; break;
    case KITTY_KP_UP : sym = SDLK_KP_8; break;
    case KITTY_KP_DOWN : sym = SDLK_KP_2; break;
    case KITTY_KP_PAGE_UP : sym = SDLK_KP_9; break;
    case KITTY_KP_PAGE_DOWN : sym = SDLK_KP_3; break;
    case KITTY_KP_HOME : sym = SDLK_KP_7; break;
    case KITTY_KP_END : sym = SDLK_KP_1; break;
    case KITTY_KP_INSERT : sym = SDLK_KP_0; break;
    case KITTY_KP_BEGIN : sym = SDLK_KP_5; break;
    default : break;
  }

  if (sym == SDLK_UNKNOWN) {
    if (key >= KITTY_KP_0 && key <= KITTY_KP_9) {
      static const SDL_Keycode kp[] = {SDLK_KP_0, SDLK_KP_1, SDLK_KP_2, SDLK_KP_3, SDLK_KP_4,
                                       SDLK_KP_5, SDLK_KP_6, SDLK_KP_7, SDLK_KP_8, SDLK_KP_9};
      sym                           = kp[ key - KITTY_KP_0 ];
    } else if (key >= TK_F1 && key <= TK_F12) {
      static const SDL_Keycode fk[] = {SDLK_F1, SDLK_F2, SDLK_F3, SDLK_F4,  SDLK_F5,  SDLK_F6,
                                       SDLK_F7, SDLK_F8, SDLK_F9, SDLK_F10, SDLK_F11, SDLK_F12};
      sym                           = fk[ key - TK_F1 ];
    } else if (key >= 'A' && key <= 'Z') {
      sym = key - 'A' + 'a';
      mod |= KMOD_LSHIFT;
    } else if (key > 32 && key < 127) {
      //
      // A shifted symbol, as SDL would see it on a US keyboard: the key
      // under it, with shift.
      //
      static const char shifted[]   = "!@#$%^&*()_+{}|:\"<>?~";
      static const char unshifted[] = "1234567890-=[]\\;',./`";
      const char       *p           = strchr(shifted, key);
      if (p) {
        sym = unshifted[ p - shifted ];
        mod |= KMOD_LSHIFT;
      } else {
        sym = key;
      }
    } else {
      return false;
    }
  }

  sc = SDL_GetScancodeFromKey(sym);

  ks->sym      = sym;
  ks->scancode = sc;
  ks->mod      = mod;
  return true;
}

static void term_push_key(const SDL_Keysym &ks, bool down, bool repeat)
{
  SDL_Event e;
  memset(&e, 0, sizeof(e));
  e.type           = down ? SDL_KEYDOWN : SDL_KEYUP;
  e.key.state      = down ? SDL_PRESSED : SDL_RELEASED;
  e.key.repeat     = repeat ? 1 : 0;
  e.key.keysym     = ks;
  e.key.timestamp  = SDL_GetTicks();
  if (sdl.window) {
    e.key.windowID = SDL_GetWindowID(sdl.window);
  }
  SDL_PushEvent(&e);
}

//
// event: 1 press, 2 repeat, 3 release
//
static void term_key_event(int key, int mods, int event)
{
  //
  // Ctrl-C quits, as it would in any terminal program.
  //
  if ((key == 'c' || key == 3) && (mods & 4) && event != 3) {
    SDL_Event e;
    memset(&e, 0, sizeof(e));
    e.type = SDL_QUIT;
    SDL_PushEvent(&e);
    return;
  }

  SDL_Keysym ks;
  if (! term_key_to_sdl(key, term_mods_to_sdl(mods), &ks)) {
    return;
  }

  int sc = ks.scancode;
  if (sc <= 0 || sc >= SDL_NUM_SCANCODES) {
    return;
  }

  if (event == 3) {
    if (term_keys[ sc ]) {
      term_keys[ sc ] = 0;
      term_push_key(ks, false, false);
    }
    return;
  }

  bool repeat = (event == 2) || term_keys[ sc ];

  //
  // A different key with this scancode still held (say shift changed): let it go first
  //
  if (term_keys[ sc ] && term_key_sym[ sc ].mod != ks.mod) {
    term_push_key(term_key_sym[ sc ], false, false);
    repeat = false;
  }

  term_keys[ sc ]    = 1;
  term_key_sym[ sc ] = ks;
  if (! term_kitty_keys) {
    term_key_release_at[ sc ] = term_now_ms() + TERM_KEY_HOLD_MS;
  }
  term_push_key(ks, true, repeat);
}

//
// Without the kitty protocol there are no releases: let keys go when their
// hold runs out.
//
static void term_release_expired_keys(void)
{
  if (term_kitty_keys) {
    return;
  }
  uint64_t now = term_now_ms();
  for (int sc = 0; sc < SDL_NUM_SCANCODES; sc++) {
    if (term_keys[ sc ] && now >= term_key_release_at[ sc ]) {
      term_keys[ sc ] = 0;
      term_push_key(term_key_sym[ sc ], false, false);
    }
  }
}

////////////////////////////////////////////////////////////////////////////
// Mouse
////////////////////////////////////////////////////////////////////////////

static void term_mouse_event(int b, int x, int y, bool release)
{
  //
  // Terminal cells (1-based) to ascii cells
  //
  int cx = x - 1 - term_off_x;
  int cy = y - 1 - term_off_y;
  cx     = std::max(0, std::min(cx, TERM_WIDTH - 1));
  cy     = std::max(0, std::min(cy, TERM_HEIGHT - 1));

  bool moved    = (cx != term_mouse_cx) || (cy != term_mouse_cy);
  term_mouse_cx = cx;
  term_mouse_cy = cy;

  int px;
  int py;
  terminal_mouse_state(&px, &py);

  SDL_Event e;
  memset(&e, 0, sizeof(e));

  if (b & 64) {
    //
    // Wheel
    //
    e.type      = SDL_MOUSEWHEEL;
    e.wheel.x   = ((b & 3) == 2) ? -1 : ((b & 3) == 3) ? 1 : 0;
    e.wheel.y   = ((b & 3) == 0) ? 1 : ((b & 3) == 1) ? -1 : 0;
    e.wheel.direction = SDL_MOUSEWHEEL_NORMAL;
    SDL_PushEvent(&e);
    return;
  }

  int button = 0;
  switch (b & 3) {
    case 0 : button = SDL_BUTTON_LEFT; break;
    case 1 : button = SDL_BUTTON_MIDDLE; break;
    case 2 : button = SDL_BUTTON_RIGHT; break;
    default : button = 0; break;
  }

  if (b & 32) {
    //
    // Motion, with or without a button held
    //
    if (moved) {
      e.type         = SDL_MOUSEMOTION;
      e.motion.x     = px;
      e.motion.y     = py;
      e.motion.state = term_mouse_buttons;
      SDL_PushEvent(&e);
    }
    return;
  }

  if (! button) {
    return;
  }

  if (release) {
    term_mouse_buttons &= ~SDL_BUTTON(button);
  } else {
    term_mouse_buttons |= SDL_BUTTON(button);
  }

  if (moved) {
    SDL_Event m;
    memset(&m, 0, sizeof(m));
    m.type         = SDL_MOUSEMOTION;
    m.motion.x     = px;
    m.motion.y     = py;
    m.motion.state = term_mouse_buttons;
    SDL_PushEvent(&m);
  }

  e.type          = release ? SDL_MOUSEBUTTONUP : SDL_MOUSEBUTTONDOWN;
  e.button.button = button;
  e.button.state  = release ? SDL_RELEASED : SDL_PRESSED;
  e.button.clicks = 1;
  e.button.x      = px;
  e.button.y      = py;
  SDL_PushEvent(&e);
}

////////////////////////////////////////////////////////////////////////////
// Parsing
////////////////////////////////////////////////////////////////////////////

//
// Splits "a:b;c:d" into up to 4 params of up to 3 sub params each.
//
static void term_parse_params(const std::string &s, int p[ 4 ][ 3 ], int *np)
{
  memset(p, 0, sizeof(int) * 12);
  int i = 0;
  int j = 0;
  *np   = s.empty() ? 0 : 1;
  for (char c : s) {
    if (c == ';') {
      if (i < 3) {
        i++;
      }
      j = 0;
      (*np)++;
    } else if (c == ':') {
      if (j < 2) {
        j++;
      }
    } else if (c >= '0' && c <= '9') {
      p[ i ][ j ] = p[ i ][ j ] * 10 + (c - '0');
    }
  }
}

//
// Handles one CSI sequence: prefix (like '<' or '?'), params, final byte.
//
static void term_csi(char prefix, const std::string &params, char final)
{
  int p[ 4 ][ 3 ];
  int np;
  term_parse_params(params, p, &np);

  //
  // SGR mouse
  //
  if (prefix == '<' && (final == 'M' || final == 'm')) {
    term_mouse_event(p[ 0 ][ 0 ], p[ 1 ][ 0 ], p[ 2 ][ 0 ], final == 'm');
    return;
  }

  //
  // Kitty protocol flags reply: the terminal has it
  //
  if (prefix == '?' && final == 'u') {
    term_kitty_keys = true;
    return;
  }

  //
  // Device attributes reply, and anything else with a prefix
  //
  if (prefix) {
    return;
  }

  int mods  = (np >= 2 && p[ 1 ][ 0 ]) ? p[ 1 ][ 0 ] - 1 : 0;
  int event = (np >= 2 && p[ 1 ][ 1 ]) ? p[ 1 ][ 1 ] : 1;

  int key = TK_NONE;
  switch (final) {
    case 'u' : key = p[ 0 ][ 0 ]; break;
    case 'A' : key = TK_UP; break;
    case 'B' : key = TK_DOWN; break;
    case 'C' : key = TK_RIGHT; break;
    case 'D' : key = TK_LEFT; break;
    case 'H' : key = TK_HOME; break;
    case 'F' : key = TK_END; break;
    case 'E' : key = KITTY_KP_BEGIN; break;
    case 'P' : key = TK_F1; break;
    case 'Q' : key = TK_F1 + 1; break;
    case 'S' : key = TK_F1 + 3; break;
    case 'Z' :
      key  = 9;
      mods |= 1;
      break;
    case '~' :
      switch (p[ 0 ][ 0 ]) {
        case 1 :
        case 7 : key = TK_HOME; break;
        case 4 :
        case 8 : key = TK_END; break;
        case 2 : key = TK_INSERT; break;
        case 3 : key = TK_DELETE; break;
        case 5 : key = TK_PAGEUP; break;
        case 6 : key = TK_PAGEDOWN; break;
        case 11 : key = TK_F1; break;
        case 12 : key = TK_F1 + 1; break;
        case 13 : key = TK_F1 + 2; break;
        case 14 : key = TK_F1 + 3; break;
        case 15 : key = TK_F1 + 4; break;
        case 17 : key = TK_F1 + 5; break;
        case 18 : key = TK_F1 + 6; break;
        case 19 : key = TK_F1 + 7; break;
        case 20 : key = TK_F1 + 8; break;
        case 21 : key = TK_F1 + 9; break;
        case 23 : key = TK_F1 + 10; break;
        case 24 : key = TK_F1 + 11; break;
        default : break;
      }
      break;
    default : break;
  }

  if (key != TK_NONE) {
    term_key_event(key, mods, event);
  }
}

//
// Consumes complete input from term_in; leaves an incomplete escape sequence
// in place for the next read.
//
static void term_parse_input(bool flush_lone_esc)
{
  size_t i = 0;
  while (i < term_in.size()) {
    unsigned char c = term_in[ i ];

    if (c != 0x1b) {
      //
      // Plain byte, or a UTF-8 sequence we take as one key
      //
      int    key = c;
      size_t len = 1;
      if (c >= 0xc0) {
        len = (c >= 0xf0) ? 4 : (c >= 0xe0) ? 3 : 2;
        if (i + len > term_in.size()) {
          break;
        }
        key = 0;
      }

      if (c == 13 || c == 10) {
        term_key_event(13, 0, 1);
      } else if (c == 9) {
        term_key_event(9, 0, 1);
      } else if (c == 127 || c == 8) {
        term_key_event(127, 0, 1);
      } else if (c >= 1 && c <= 26) {
        term_key_event('a' + c - 1, 4, 1);
      } else if (key) {
        term_key_event(key, 0, 1);
      }
      i += len;
      continue;
    }

    //
    // ESC
    //
    if (i + 1 >= term_in.size()) {
      if (flush_lone_esc) {
        term_key_event(27, 0, 1);
        i++;
        continue;
      }
      break;
    }

    unsigned char n = term_in[ i + 1 ];

    if (n == '[') {
      //
      // CSI: optional prefix, params, final byte 0x40..0x7e
      //
      size_t j      = i + 2;
      char   prefix = 0;
      if (j < term_in.size() && strchr("<?>=", term_in[ j ])) {
        prefix = term_in[ j ];
        j++;
      }
      size_t start = j;
      while (j < term_in.size() && ! (term_in[ j ] >= 0x40 && term_in[ j ] <= 0x7e)) {
        j++;
      }
      if (j >= term_in.size()) {
        break;
      }
      term_csi(prefix, term_in.substr(start, j - start), term_in[ j ]);
      i = j + 1;
      continue;
    }

    if (n == 'O') {
      //
      // SS3: F1 to F4, and arrows in application mode
      //
      if (i + 2 >= term_in.size()) {
        break;
      }
      term_csi(0, "", term_in[ i + 2 ]);
      i += 3;
      continue;
    }

    //
    // Alt + key
    //
    if (n >= 32 && n < 127) {
      term_key_event(n, 2, 1);
      i += 2;
      continue;
    }

    term_key_event(27, 0, 1);
    i++;
  }

  term_in.erase(0, i);
}

void terminal_poll(void)
{
  if (! term_started) {
    return;
  }

  char buf[ 4096 ];
  for (;;) {
    ssize_t n = read(STDIN_FILENO, buf, sizeof(buf));
    if (n <= 0) {
      break;
    }
    term_in.append(buf, (size_t) n);
  }

  if (! term_in.empty()) {
    bool waiting_on_esc = (term_in.back() == 0x1b) || (term_in.size() >= 2 && term_in[ term_in.size() - 2 ] == 0x1b);
    if (waiting_on_esc && ! term_in_esc_at) {
      term_in_esc_at = term_now_ms();
    }
    bool flush = term_in_esc_at && (term_now_ms() - term_in_esc_at >= TERM_ESC_WAIT_MS);
    term_parse_input(flush);
    if (term_in.empty()) {
      term_in_esc_at = 0;
    }
  }

  term_release_expired_keys();

  if (term_resized) {
    term_resized = false;
    term_read_size();
    term_screen_valid = false;
  }
}

////////////////////////////////////////////////////////////////////////////
// Output
////////////////////////////////////////////////////////////////////////////

static void term_put_utf8(std::string &out, wchar_t ch)
{
  uint32_t c = (uint32_t) ch;
  if (c < 0x80) {
    out += (char) c;
  } else if (c < 0x800) {
    out += (char) (0xc0 | (c >> 6));
    out += (char) (0x80 | (c & 0x3f));
  } else if (c < 0x10000) {
    out += (char) (0xe0 | (c >> 12));
    out += (char) (0x80 | ((c >> 6) & 0x3f));
    out += (char) (0x80 | (c & 0x3f));
  } else {
    out += (char) (0xf0 | (c >> 18));
    out += (char) (0x80 | ((c >> 12) & 0x3f));
    out += (char) (0x80 | ((c >> 6) & 0x3f));
    out += (char) (0x80 | (c & 0x3f));
  }
}

void terminal_display(void)
{
  if (! term_started) {
    return;
  }

  //
  // About 60 frames a second is plenty for a terminal
  //
  uint64_t now = term_now_ms();
  if (term_screen_valid && (now - term_last_frame_ms < 16)) {
    return;
  }
  term_last_frame_ms = now;

  int w = std::min((int) TERM_WIDTH, term_cols);
  int h = std::min((int) TERM_HEIGHT, term_rows);

  int off_x = std::max(0, (term_cols - TERM_WIDTH) / 2);
  int off_y = std::max(0, (term_rows - TERM_HEIGHT) / 2);

  std::string out;
  out.reserve(64 * 1024);

  if (! term_screen_valid || off_x != term_off_x || off_y != term_off_y) {
    out += "\x1b[0m\x1b[48;2;0;0;0m\x1b[2J";
    for (int y = 0; y < TERM_HEIGHT_MAX; y++) {
      for (int x = 0; x < TERM_WIDTH_MAX; x++) {
        term_screen[ y ][ x ].ch = 0;
      }
    }
    term_off_x        = off_x;
    term_off_y        = off_y;
    term_screen_valid = true;
  }

  bool  have_pen = false;
  color pen_fg;
  color pen_bg;
  int   cur_x = -1;
  int   cur_y = -1;
  char  esc[ 64 ];

  for (int y = 0; y < h; y++) {
    for (int x = 0; x < w; x++) {
      wchar_t ch;
      color   fg;
      color   bg;
      ascii_cell_resolve(x, y, &ch, &fg, &bg);

      if (fg == COLOR_NONE) {
        fg = WHITE;
      }
      if (bg == COLOR_NONE) {
        bg = BLACK;
      }

      //
      // One column per cell, always
      //
      if (ch < 32 || ch == 127) {
        ch = ' ';
      } else if (ch >= 127) {
        int cw = wcwidth(ch);
        if (cw == 0 || cw == 2) {
          ch = '?';
        }
      }

      TermCell &t = term_screen[ y ][ x ];
      if (t.ch == ch && t.fg == fg && t.bg == bg) {
        continue;
      }
      t.ch = ch;
      t.fg = fg;
      t.bg = bg;

      if (cur_x != x || cur_y != y) {
        snprintf(esc, sizeof(esc), "\x1b[%d;%dH", y + 1 + off_y, x + 1 + off_x);
        out += esc;
      }

      if (! have_pen || ! (pen_fg == fg)) {
        snprintf(esc, sizeof(esc), "\x1b[38;2;%u;%u;%um", fg.r, fg.g, fg.b);
        out += esc;
        pen_fg = fg;
      }
      if (! have_pen || ! (pen_bg == bg)) {
        snprintf(esc, sizeof(esc), "\x1b[48;2;%u;%u;%um", bg.r, bg.g, bg.b);
        out += esc;
        pen_bg = bg;
      }
      have_pen = true;

      term_put_utf8(out, ch);

      //
      // Terminals disagree on the width of some characters; never trust the
      // cursor to be where we think after anything but ASCII.
      //
      if (ch < 127) {
        cur_x = x + 1;
        cur_y = y;
      } else {
        cur_x = -1;
      }
    }
  }

  if (! out.empty()) {
    term_write(out);
  }
}
