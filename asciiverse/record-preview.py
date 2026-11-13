#!/usr/bin/env python3
"""Records the catalog preview, asciiverse/zorbash.cast (100x30, asciinema
3): the title screen, a new game's dungeon map, then the first levels played
by the game's own robot (Robo mode, slowed by --robot-delay to about four
steps a second, as a person plays) for up to 42 s, or until it falls and
its tombstone has been seen, then Ctrl-C quits.

asciinema runs in a pseudo-terminal of this script's own, which answers the
game like a terminal with the kitty keyboard protocol. Keys are sent on a
timetable; the robot does the playing. Waits longer than two seconds (the
level being made) are cut by asciinema's idle limit.

    python3 asciiverse/record-preview.py [--dry] [--seed NAME]
      --dry    run the game without recording
      --seed   the dungeon seed (default below; the robot fights by chance, so
               a take can end early: the script says how long it played)

Plays the staged game (stage/zorbash, from asciiverse/stage.sh) with sound
off, a scratch HOME and state folder, and a neutral player name."""
import fcntl, os, pty, select, struct, sys, tempfile, termios, time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
STAGE = os.path.join(ROOT, "stage")
CAST = os.path.join(ROOT, "asciiverse", "zorbash.cast")
W, H = 100, 30
dry = "--dry" in sys.argv
seed = sys.argv[sys.argv.index("--seed") + 1] if "--seed" in sys.argv else "amberhollow"

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
game = "zorbash --terminal --silent --robot-delay 50 --seed %s" % seed
argv = (["sh", "-c", "exec " + game] if dry else
        ["asciinema", "rec", "-q", "--overwrite", "--window-size", "%dx%d" % (W, H),
         "--idle-time-limit", "2", "--capture-env", "TERM,COLORTERM,LANG",
         "--title", "Zorbash", "--command", game, CAST])

pid, fd = pty.fork()
if pid == 0:
    # size the terminal before anything starts in it, so nothing sees another size
    fcntl.ioctl(0, termios.TIOCSWINSZ, struct.pack("HHHH", H, W, 0, 0))
    os.chdir(home)
    os.execvpe(argv[0], argv, env)


seen = bytearray()


def pump(t, until=None):
    """Reads the game's output for t seconds (or until a text appears),
    answering its terminal queries."""
    end = time.time() + t
    while time.time() < end:
        if until is not None and until in seen:
            return True
        r, _, _ = select.select([fd], [], [], 0.01)
        if r:
            try:
                data = os.read(fd, 65536)
            except OSError:
                return False
            if not data:
                return False
            seen.extend(data)
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
del seen[:]
key(13)                   # Enter the Dungeon (the level is made: cut to 2 s)
pump(20.0, until=b"Welcome to the lair")
pump(1.5)                 # the first room
del seen[:]
key(ord("r"))             # Robo mode: the game plays itself
robot = time.time()
if pump(42.0, until=b"RIP:") and b"RIP:" in seen:
    pump(3.5)             # the robot fell: its tombstone, then the end
print("robot played %.1f s" % (time.time() - robot))
os.write(fd, b"\x1b[99;5u")  # Ctrl-C: quits, the terminal restored
pump(4.0)
try:
    os.waitpid(pid, 0)
except ChildProcessError:
    pass
if not dry:
    print("wrote", CAST)
