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
// Windows (_WIN32): the same output, through the console's own escape
// sequence support (Windows 10 and later). Keys, with real releases, and the
// mouse come from the console's input records instead of escape sequences,
// and go to the same handlers. Everything Windows has its own section below.
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
#include <sys/stat.h>
#include <time.h>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <io.h>
#include <windows.h>
#else
#include <poll.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>
#endif

bool g_opt_terminal;

static bool            term_started;
static int             term_fd = -1;
static int             term_saved_stderr = -1;
static bool            term_redirected;
#ifdef _WIN32
static HANDLE          term_con_in  = INVALID_HANDLE_VALUE;
static HANDLE          term_con_out = INVALID_HANDLE_VALUE;
static DWORD           term_saved_in_mode;
static DWORD           term_saved_out_mode;
static int             term_win_left;
static int             term_win_top;
#else
static bool            term_have_saved_termios;
static struct termios  term_saved_termios;
#endif
static int             term_cols = 80;
static int             term_rows = 24;
static volatile bool   term_resized;
static bool            term_layout_changed;
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
static Config         *term_file_config;
static bool            term_file_ascii;

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
#ifdef _WIN32
  return (uint64_t) GetTickCount64();
#else
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t) ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
#endif
}

#ifdef _WIN32
//
// The console takes UTF-16 whatever its code page is
//
static void term_write(const std::string &s)
{
  if (s.empty()) {
    return;
  }
  int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int) s.size(), nullptr, 0);
  if (n <= 0) {
    return;
  }
  std::wstring w((size_t) n, L' ');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), (int) s.size(), &w[ 0 ], n);
  const wchar_t *p = w.data();
  DWORD          left = (DWORD) n;
  while (left) {
    DWORD done = 0;
    if (! WriteConsoleW(term_con_out, p, left, &done, nullptr) || ! done) {
      return;
    }
    p += done;
    left -= done;
  }
}

static void term_read_size(void)
{
  CONSOLE_SCREEN_BUFFER_INFO info;
  if (GetConsoleScreenBufferInfo(term_con_out, &info)) {
    int cols = info.srWindow.Right - info.srWindow.Left + 1;
    int rows = info.srWindow.Bottom - info.srWindow.Top + 1;
    if (cols > 0 && rows > 0) {
      term_cols = cols;
      term_rows = rows;
    }
    term_win_left = info.srWindow.Left;
    term_win_top  = info.srWindow.Top;
  }
}
#else
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
#endif

bool terminal_take_layout_change(void)
{
  bool changed        = term_layout_changed;
  term_layout_changed = false;
  return changed;
}

int terminal_cols(void) { return std::min(term_cols, (int) TERM_WIDTH_MAX); }
int terminal_rows(void) { return std::min(term_rows, (int) TERM_HEIGHT_MAX); }

#ifdef _WIN32
//
// Windows always has APPDATA, which main.cpp uses before asking here; without
// it, the game's own default (a folder beside it) stands.
//
std::string terminal_state_dir(void) { return ""; }
#else
//
// mkdir -p
//
static void term_mkdirs(const std::string &path)
{
  for (size_t i = 1; i <= path.size(); i++) {
    if (i == path.size() || path[ i ] == '/') {
      mkdir(path.substr(0, i).c_str(), 0700);
    }
  }
}

std::string terminal_state_dir(void)
{
  std::string dir;
  const char *xdg = getenv("XDG_STATE_HOME");
  if (xdg && xdg[ 0 ] == '/') {
    dir = xdg;
  } else {
    const char *home = getenv("HOME");
    if (! home || ! home[ 0 ]) {
      return "";
    }
#ifdef __APPLE__
    dir = std::string(home) + "/Library/Application Support";
#else
    dir = std::string(home) + "/.local/state";
#endif
  }
  term_mkdirs(dir);
  return dir;
}
#endif

#ifdef _WIN32
bool terminal_init(void)
{
  DWORD mode;
  if (! GetConsoleMode(GetStdHandle(STD_INPUT_HANDLE), &mode)
      || ! GetConsoleMode(GetStdHandle(STD_OUTPUT_HANDLE), &mode)) {
    return false;
  }

  //
  // Our own handles on the console, which stay put when stdout is sent
  // elsewhere below
  //
  term_con_in  = CreateFileW(L"CONIN$", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                             OPEN_EXISTING, 0, nullptr);
  term_con_out = CreateFileW(L"CONOUT$", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                             OPEN_EXISTING, 0, nullptr);
  if (term_con_in == INVALID_HANDLE_VALUE || term_con_out == INVALID_HANDLE_VALUE) {
    return false;
  }

  //
  // Escape sequences: Windows 10 and later
  //
  GetConsoleMode(term_con_out, &term_saved_out_mode);
  if (! SetConsoleMode(term_con_out, term_saved_out_mode | ENABLE_PROCESSED_OUTPUT | ENABLE_VIRTUAL_TERMINAL_PROCESSING)) {
    return false;
  }
  SetConsoleMode(term_con_out, term_saved_out_mode);

  //
  // Everything else the game prints goes nowhere until its log files are
  // open, then into them: the console shows only the game.
  //
  term_fd           = _dup(1);
  term_saved_stderr = _dup(2);
  fflush(stdout);
  fflush(stderr);
  int devnull = _open("NUL", _O_WRONLY);
  if (devnull >= 0) {
    _dup2(devnull, 1);
    _dup2(devnull, 2);
    _close(devnull);
  }
  term_redirected = true;
  atexit(terminal_fini);

  term_read_size();
  return true;
}

void terminal_start(void)
{
  if (term_started) {
    return;
  }

  //
  // Keys and mouse as input records: no line editing, echo, Ctrl-C handling
  // or quick edit selection
  //
  GetConsoleMode(term_con_in, &term_saved_in_mode);
  SetConsoleMode(term_con_in, ENABLE_EXTENDED_FLAGS | ENABLE_WINDOW_INPUT | ENABLE_MOUSE_INPUT);
  GetConsoleMode(term_con_out, &term_saved_out_mode);
  SetConsoleMode(term_con_out,
                 ENABLE_PROCESSED_OUTPUT | ENABLE_VIRTUAL_TERMINAL_PROCESSING | DISABLE_NEWLINE_AUTO_RETURN);
  FlushConsoleInputBuffer(term_con_in);

  //
  // Stray prints from the game go to its log files
  //
  if (g_log_stdout) {
    fflush(stdout);
    _dup2(_fileno(g_log_stdout), 1);
  }
  if (g_log_stderr) {
    fflush(stderr);
    _dup2(_fileno(g_log_stderr), 2);
  }

  //
  // Input records tell of key releases, as the kitty protocol does
  //
  term_kitty_keys = true;
  term_started    = true;

  //
  // Alternate screen, hide cursor, no autowrap
  //
  term_write("\x1b[?1049h\x1b[?25l\x1b[?7l\x1b[2J");

  term_screen_valid = false;
}

void terminal_fini(void)
{
  if (term_redirected) {
    term_redirected = false;
    _dup2(term_fd, 1);
    if (term_saved_stderr >= 0) {
      _dup2(term_saved_stderr, 2);
    }
  }

  if (! term_started) {
    return;
  }
  term_started = false;

  static const char reset[] = "\x1b[0m\x1b[?7h\x1b[?25h\x1b[?1049l";
  DWORD             unused;
  WriteConsoleA(term_con_out, reset, sizeof(reset) - 1, &unused, nullptr);

  SetConsoleMode(term_con_out, term_saved_out_mode);
  SetConsoleMode(term_con_in, term_saved_in_mode);
}
#else
bool terminal_init(void)
{
  if (! isatty(STDIN_FILENO) || ! isatty(STDOUT_FILENO)) {
    return false;
  }

  //
  // For wcwidth on the characters we write
  //
  setlocale(LC_CTYPE, "");

  //
  // Our own handle on the terminal. Everything else the game prints goes
  // nowhere until its log files are open, then into them: the terminal shows
  // only the game.
  //
  term_fd           = dup(STDOUT_FILENO);
  term_saved_stderr = dup(STDERR_FILENO);
  fflush(stdout);
  fflush(stderr);
  int devnull = open("/dev/null", O_WRONLY);
  if (devnull >= 0) {
    dup2(devnull, STDOUT_FILENO);
    dup2(devnull, STDERR_FILENO);
    close(devnull);
  }
  term_redirected = true;
  atexit(terminal_fini);

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
  // Stray prints from the game go to its log files
  //
  if (g_log_stdout) {
    fflush(stdout);
    dup2(fileno(g_log_stdout), STDOUT_FILENO);
  }
  if (g_log_stderr) {
    fflush(stderr);
    dup2(fileno(g_log_stderr), STDERR_FILENO);
  }

  term_started = true;

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
  //
  // Only async-signal-safe calls in here, as this runs from the crash handler too
  //
  if (term_redirected) {
    term_redirected = false;
    dup2(term_fd, STDOUT_FILENO);
    if (term_saved_stderr >= 0) {
      dup2(term_saved_stderr, STDERR_FILENO);
    }
  }

  if (! term_started) {
    return;
  }
  term_started = false;

  static const char reset[] = "\x1b[<u\x1b[?1003l\x1b[?1006l\x1b[0m\x1b[?7h\x1b[?25h\x1b[?1049l";
  ssize_t           unused  = write(term_fd, reset, sizeof(reset) - 1);
  (void) unused;

  if (term_have_saved_termios) {
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &term_saved_termios);
  }

  int flags = fcntl(STDIN_FILENO, F_GETFL, 0);
  fcntl(STDIN_FILENO, F_SETFL, flags & ~O_NONBLOCK);
}
#endif

void terminal_remember_config(void)
{
  delete term_file_config;
  term_file_config = new Config(game->config);
  term_file_ascii  = g_opt_ascii;
}

void terminal_config_for_saving(Config &c, bool *ascii_mode)
{
  if (! term_file_config) {
    return;
  }
  const Config &f = *term_file_config;

  c.ascii_gl_height       = f.ascii_gl_height;
  c.ascii_gl_width        = f.ascii_gl_width;
  c.config_pix_height     = f.config_pix_height;
  c.config_pix_width      = f.config_pix_width;
  c.game_pix_height       = f.game_pix_height;
  c.game_pix_scale_height = f.game_pix_scale_height;
  c.game_pix_scale_width  = f.game_pix_scale_width;
  c.game_pix_width        = f.game_pix_width;
  c.gfx_vsync_locked      = f.gfx_vsync_locked;
  c.one_pixel_height      = f.one_pixel_height;
  c.one_pixel_width       = f.one_pixel_width;
  c.tile_pix_height       = f.tile_pix_height;
  c.tile_pix_width        = f.tile_pix_width;
  c.tile_pixel_height     = f.tile_pixel_height;
  c.tile_pixel_width      = f.tile_pixel_width;
  c.ui_pix_height         = f.ui_pix_height;
  c.ui_pix_width          = f.ui_pix_width;
  c.video_w_h_ratio       = f.video_w_h_ratio;
  c.window_pix_height     = f.window_pix_height;
  c.window_pix_width      = f.window_pix_width;

  *ascii_mode = term_file_ascii;
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

  //
  // The game's own table: SDL's keymap only exists when SDL video runs,
  // which it does not in terminal mode.
  //
  ks->sym = sym;
  sc      = sdlk_to_scancode(*ks);

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

//
// After a resize: draw everything again, and lay the UI out again if the
// grid changed size
//
static void term_take_resize(void)
{
  int old_cols = terminal_cols();
  int old_rows = terminal_rows();
  term_read_size();
  term_screen_valid = false;
  if (terminal_cols() != old_cols || terminal_rows() != old_rows) {
    term_layout_changed = true;
  }
}

#ifdef _WIN32
////////////////////////////////////////////////////////////////////////////
// Windows console input
////////////////////////////////////////////////////////////////////////////

//
// The key and modifiers each held virtual key went down as, so its release
// lets go of the same key whatever the modifiers are by then
//
static int      term_vk_key[ 256 ];
static int      term_vk_mods[ 256 ];
static DWORD    term_win_buttons;
static uint64_t term_size_checked_at;

static void term_win_release_all(void)
{
  for (int vk = 0; vk < 256; vk++) {
    if (term_vk_key[ vk ]) {
      term_key_event(term_vk_key[ vk ], term_vk_mods[ vk ], 3);
      term_vk_key[ vk ] = 0;
    }
  }
}

static void term_win_key(const KEY_EVENT_RECORD &k)
{
  int vk = k.wVirtualKeyCode & 0xff;

  if (! k.bKeyDown) {
    if (term_vk_key[ vk ]) {
      term_key_event(term_vk_key[ vk ], term_vk_mods[ vk ], 3);
      term_vk_key[ vk ] = 0;
    }
    return;
  }

  DWORD st   = k.dwControlKeyState;
  int   mods = 0;
  if (st & SHIFT_PRESSED) {
    mods |= 1;
  }
  if (st & (LEFT_ALT_PRESSED | RIGHT_ALT_PRESSED)) {
    mods |= 2;
  }
  if (st & (LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED)) {
    mods |= 4;
  }

  //
  // The arrows, Home, End and so on of the keypad, with num lock off, are
  // the ones without the enhanced flag
  //
  bool kp  = ! (st & ENHANCED_KEY);
  int  key = TK_NONE;
  switch (vk) {
    case VK_UP : key = kp ? KITTY_KP_UP : TK_UP; break;
    case VK_DOWN : key = kp ? KITTY_KP_DOWN : TK_DOWN; break;
    case VK_LEFT : key = kp ? KITTY_KP_LEFT : TK_LEFT; break;
    case VK_RIGHT : key = kp ? KITTY_KP_RIGHT : TK_RIGHT; break;
    case VK_HOME : key = kp ? KITTY_KP_HOME : TK_HOME; break;
    case VK_END : key = kp ? KITTY_KP_END : TK_END; break;
    case VK_PRIOR : key = kp ? KITTY_KP_PAGE_UP : TK_PAGEUP; break;
    case VK_NEXT : key = kp ? KITTY_KP_PAGE_DOWN : TK_PAGEDOWN; break;
    case VK_INSERT : key = kp ? KITTY_KP_INSERT : TK_INSERT; break;
    case VK_DELETE : key = kp ? KITTY_KP_DELETE : TK_DELETE; break;
    case VK_CLEAR : key = KITTY_KP_BEGIN; break;
    case VK_RETURN : key = kp ? 13 : KITTY_KP_ENTER; break;
    case VK_ESCAPE : key = 27; break;
    case VK_TAB : key = 9; break;
    case VK_BACK : key = 127; break;
    case VK_SPACE : key = 32; break;
    case VK_DECIMAL : key = KITTY_KP_DECIMAL; break;
    case VK_DIVIDE : key = KITTY_KP_DIVIDE; break;
    case VK_MULTIPLY : key = KITTY_KP_MULTIPLY; break;
    case VK_SUBTRACT : key = KITTY_KP_SUBTRACT; break;
    case VK_ADD : key = KITTY_KP_ADD; break;
    default :
      if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) {
        key = KITTY_KP_0 + (vk - VK_NUMPAD0);
      } else if (vk >= VK_F1 && vk <= VK_F12) {
        key = TK_F1 + (vk - VK_F1);
      } else {
        int c = k.uChar.UnicodeChar;
        if (c >= 1 && c <= 26) {
          //
          // Ctrl + letter
          //
          key = 'a' + c - 1;
          mods |= 4;
        } else if (c >= 32) {
          //
          // The character typed, whatever the keyboard layout: it has shift
          // in it already, and AltGr is not Ctrl + Alt
          //
          key = c;
          mods &= ~1;
          if ((st & RIGHT_ALT_PRESSED) && (st & LEFT_CTRL_PRESSED)) {
            mods &= ~6;
          }
        } else if ((vk >= '0' && vk <= '9') || (vk >= 'A' && vk <= 'Z')) {
          //
          // Ctrl + digit and the like, which have no character
          //
          key = (vk >= 'A') ? vk - 'A' + 'a' : vk;
        }
      }
      break;
  }

  if (key == TK_NONE) {
    return;
  }

  int event = 1;
  if (term_vk_key[ vk ]) {
    if (term_vk_key[ vk ] == key && term_vk_mods[ vk ] == mods) {
      event = 2;
    } else {
      term_key_event(term_vk_key[ vk ], term_vk_mods[ vk ], 3);
    }
  }
  term_vk_key[ vk ]  = key;
  term_vk_mods[ vk ] = mods;
  term_key_event(key, mods, event);
}

//
// Mouse records to the SGR button codes term_mouse_event takes
//
static void term_win_mouse(const MOUSE_EVENT_RECORD &m)
{
  int x = m.dwMousePosition.X - term_win_left + 1;
  int y = m.dwMousePosition.Y - term_win_top + 1;

  if (m.dwEventFlags & MOUSE_WHEELED) {
    short d = (short) HIWORD(m.dwButtonState);
    term_mouse_event(64 | (d > 0 ? 0 : 1), x, y, false);
    return;
  }
  if (m.dwEventFlags & MOUSE_HWHEELED) {
    short d = (short) HIWORD(m.dwButtonState);
    term_mouse_event(64 | (d > 0 ? 3 : 2), x, y, false);
    return;
  }

  //
  // Left, middle, right
  //
  static const DWORD bits[ 3 ] = {FROM_LEFT_1ST_BUTTON_PRESSED, FROM_LEFT_2ND_BUTTON_PRESSED,
                                  RIGHTMOST_BUTTON_PRESSED};
  DWORD              now       = m.dwButtonState & (bits[ 0 ] | bits[ 1 ] | bits[ 2 ]);
  bool               changed   = false;
  for (int b = 0; b < 3; b++) {
    if ((now ^ term_win_buttons) & bits[ b ]) {
      term_mouse_event(b, x, y, ! (now & bits[ b ]));
      changed = true;
    }
  }
  term_win_buttons = now;

  if (! changed) {
    term_mouse_event(32 | 3, x, y, false);
  }
}

void terminal_wait(int ms)
{
  if (term_con_in == INVALID_HANDLE_VALUE) {
    Sleep((DWORD) ms);
    return;
  }
  WaitForSingleObject(term_con_in, (DWORD) ms);
}

void terminal_poll(void)
{
  if (! term_started) {
    return;
  }

  for (;;) {
    DWORD pending = 0;
    if (! GetNumberOfConsoleInputEvents(term_con_in, &pending) || ! pending) {
      break;
    }
    INPUT_RECORD rec[ 64 ];
    DWORD        got = 0;
    if (! ReadConsoleInputW(term_con_in, rec, 64, &got) || ! got) {
      break;
    }
    for (DWORD i = 0; i < got; i++) {
      switch (rec[ i ].EventType) {
        case KEY_EVENT : term_win_key(rec[ i ].Event.KeyEvent); break;
        case MOUSE_EVENT : term_win_mouse(rec[ i ].Event.MouseEvent); break;
        case WINDOW_BUFFER_SIZE_EVENT : term_resized = true; break;
        case FOCUS_EVENT :
          //
          // Releases made in another window never reach us
          //
          if (! rec[ i ].Event.FocusEvent.bSetFocus) {
            term_win_release_all();
          }
          break;
        default : break;
      }
    }
  }

  //
  // Not every console reports every resize: look now and then as well
  //
  uint64_t now = term_now_ms();
  if (now - term_size_checked_at >= 250) {
    term_size_checked_at = now;
    int cols             = term_cols;
    int rows             = term_rows;
    term_read_size();
    if (cols != term_cols || rows != term_rows) {
      term_cols    = cols;
      term_rows    = rows;
      term_resized = true;
    }
  }

  if (term_resized) {
    term_resized = false;
    term_take_resize();
  }
}
#else
void terminal_wait(int ms)
{
  struct pollfd pfd;
  pfd.fd      = STDIN_FILENO;
  pfd.events  = POLLIN;
  pfd.revents = 0;
  poll(&pfd, 1, ms);
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
    term_take_resize();
  }
}
#endif

////////////////////////////////////////////////////////////////////////////
// Output
////////////////////////////////////////////////////////////////////////////

#ifdef _WIN32
//
// Windows has no wcwidth: zero width for combining marks and the like, two
// for the wide East Asian ranges, one for the rest (the game's own glyphs)
//
static int term_wcwidth(wchar_t ch)
{
  unsigned c = (unsigned) ch;
  if ((c >= 0x0300 && c <= 0x036f) || (c >= 0x1ab0 && c <= 0x1aff) || (c >= 0x1dc0 && c <= 0x1dff)
      || (c >= 0x200b && c <= 0x200f) || (c >= 0x20d0 && c <= 0x20ff) || (c >= 0xd800 && c <= 0xdfff)
      || (c >= 0xfe00 && c <= 0xfe0f) || (c >= 0xfe20 && c <= 0xfe2f)) {
    return 0;
  }
  if ((c >= 0x1100 && c <= 0x115f) || (c >= 0x2e80 && c <= 0x303e) || (c >= 0x3041 && c <= 0x33ff)
      || (c >= 0x3400 && c <= 0x4dbf) || (c >= 0x4e00 && c <= 0x9fff) || (c >= 0xa000 && c <= 0xa4cf)
      || (c >= 0xac00 && c <= 0xd7a3) || (c >= 0xf900 && c <= 0xfaff) || (c >= 0xfe30 && c <= 0xfe4f)
      || (c >= 0xff00 && c <= 0xff60) || (c >= 0xffe0 && c <= 0xffe6)) {
    return 2;
  }
  return 1;
}
#else
static int term_wcwidth(wchar_t ch) { return wcwidth(ch); }
#endif

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
        int cw = term_wcwidth(ch);
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
