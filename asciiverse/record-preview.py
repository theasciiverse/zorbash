#!/usr/bin/env python3
"""Records the catalog preview, asciiverse/zorbash.cast (100x30, asciinema
3): the title screen, a new game's dungeon map, then the first levels played
by the game's own robot (Robo mode), until it quits with Ctrl-C.

asciinema runs in a pseudo-terminal of this script's own, which answers the
game like a terminal with the kitty keyboard protocol. Keys are sent on a
timetable; the robot does the playing. Waits longer than two seconds (the
level being made) are cut by asciinema's idle limit.

    python3 asciiverse/record-preview.py [--dry] [--seed NAME]
      --dry    run the game without recording
      --seed   the dungeon seed (default below, picked for a lively start)

Plays the staged game (stage/zorbash, from asciiverse/stage.sh) with sound
off, a scratch HOME and state folder, and a neutral player name."""
import fcntl, os, pty, select, struct, sys, tempfile, termios, time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
STAGE = os.path.join(ROOT, "stage")
CAST = os.path.join(ROOT, "asciiverse", "zorbash.cast")
W, H = 100, 30
dry = "--dry" in sys.argv
seed = sys.argv[sys.argv.index("--seed") + 1] if "--seed" in sys.argv else "lanternmoth"

home = tempfile.mkdtemp()
env = {
    "PATH": STAGE + ":/usr/bin:/bin",
    "HOME": home,
    "USER": "player",
    "LOGNAME": "player",
    "TERM": "xterm-256color",
    "COLORTERM": "truecolor",
    "LANG": "C.UTF-8",
    "XDG_STATE_HOME": os.path.join(home, "state"),
    "SDL_AUDIODRIVER": "dummy",
}
game = "zorbash --terminal --silent --seed %s" % seed
argv = (["sh", "-c", "exec " + game] if dry else
        ["asciinema", "rec", "-q", "--overwrite", "--window-size", "%dx%d" % (W, H),
         "--idle-time-limit", "2", "--capture-env", "TERM,COLORTERM,LANG",
         "--title", "Zorbash", "--command", game, CAST])

pid, fd = pty.fork()
if pid == 0:
    os.chdir(home)
    os.execvpe(argv[0], argv, env)
fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", H, W, 0, 0))


def pump(t):
    """Reads the game's output for t seconds, answering its terminal queries."""
    end = time.time() + t
    while time.time() < end:
        r, _, _ = select.select([fd], [], [], 0.01)
        if r:
            try:
                data = os.read(fd, 65536)
            except OSError:
                return False
            if not data:
                return False
            if b"\x1b[?u" in data:                    # kitty keyboard: yes
                os.write(fd, b"\x1b[?11u")
            if b"\x1b[c" in data:                     # device attributes
                os.write(fd, b"\x1b[?62;22c")
    return True


def key(code, hold=0.08):
    """Presses and lets go of a key (kitty protocol: press, then release)."""
    os.write(fd, b"\x1b[%du" % code)
    pump(hold)
    os.write(fd, b"\x1b[%d;1:3u" % code)


pump(3.5)                 # the title screen
key(ord("n"))             # New game: the dungeon's map
pump(3.0)
key(13)                   # Enter the Dungeon (the level is made: cut to 2 s)
pump(14.0)
pump(2.5)                 # the first room
key(ord("r"))             # Robo mode: the game plays itself
pump(42.0)
os.write(fd, b"\x1b[99;5u")  # Ctrl-C: quits, the terminal restored
pump(4.0)
try:
    os.waitpid(pid, 0)
except ChildProcessError:
    pass
if not dry:
    print("wrote", CAST)
