#!/usr/bin/env python3
"""Smoke test of a built Zorbash, played in a pseudo-terminal of 100x30:
the title screen, New game (n), Enter the Dungeon (Enter), the robot playing
(r) for a few seconds, then Ctrl-C, which must quit with exit code 0.
Passes when the game drew its title screen and the first level ("Welcome to
the lair") and quit cleanly.

    python3 asciiverse/smoke.py <program> [<program args> ...]

On Windows the pseudo-terminal is the console's own (ConPTY, through
pywinpty: pip install pywinpty), so this tests the console backend;
elsewhere a Unix pty, so `python3 asciiverse/smoke.py wine zorbash.exe`
tests the Windows build under Wine. Sound is off, and the saves and logs go
to a fresh scratch folder (APPDATA on Windows, XDG_STATE_HOME and HOME
elsewhere, and its own WINEPREFIX under Wine), printed at the end."""
import os, subprocess, sys, tempfile, threading, time

W, H = 100, 30
WINDOWS = sys.platform == "win32"
# paths made absolute: the game runs in the scratch folder
cmd = [os.path.abspath(a) if os.path.isfile(a) else a for a in sys.argv[1:]]
cmd += ["--terminal", "--silent", "--seed", "amberhollow"]
scratch = tempfile.mkdtemp(prefix="zorbash-smoke-")
env = dict(os.environ, SDL_AUDIODRIVER="dummy", APPDATA=scratch,
           HOME=scratch, XDG_STATE_HOME=os.path.join(scratch, "state"),
           WINEPREFIX=os.path.join(scratch, "wine"), WINEARCH="win64", WINEDEBUG="-all",
           TERM="xterm-256color", COLORTERM="truecolor", LANG="C.UTF-8")
seen = bytearray()
lock = threading.Lock()

if WINDOWS:
    from winpty import PtyProcess

    proc = PtyProcess.spawn(subprocess.list2cmdline(cmd), dimensions=(H, W), env=env)

    def reader():
        while True:
            try:
                data = proc.read(65536)
            except Exception:
                return
            with lock:
                seen.extend(data.encode("utf-8", "replace"))

    def send(s):
        proc.write(s)

    def alive():
        return proc.isalive()

    def code():
        return proc.exitstatus
else:
    import fcntl, pty, struct, termios

    pid, fd = pty.fork()
    if pid == 0:
        fcntl.ioctl(0, termios.TIOCSWINSZ, struct.pack("HHHH", H, W, 0, 0))
        os.chdir(scratch)
        os.execvpe(cmd[0], cmd, env)
    status = [None]

    def reader():
        while True:
            try:
                data = os.read(fd, 65536)
            except OSError:
                return
            if not data:
                return
            with lock:
                seen.extend(data)
            if b"\x1b[c" in data:  # device attributes: a plain terminal
                os.write(fd, b"\x1b[?62;22c")

    def send(s):
        os.write(fd, s.encode())

    def alive():
        if status[0] is None:
            p, st = os.waitpid(pid, os.WNOHANG)
            if p:
                status[0] = os.waitstatus_to_exitcode(st)
        return status[0] is None

    def code():
        alive()
        return status[0]

threading.Thread(target=reader, daemon=True).start()


def wait_for(texts, seconds):
    end = time.time() + seconds
    while time.time() < end and alive():
        with lock:
            if any(t in seen for t in texts):
                return True
        time.sleep(0.1)
    with lock:
        return any(t in seen for t in texts)


def fail(why):
    print("FAIL:", why, "(running)" if alive() else "(exited, code %s)" % code())
    with lock:
        tail = bytes(seen[-3000:])
    print("last output:", tail.decode("utf-8", "replace").encode("ascii", "backslashreplace").decode())
    for root, _, files in os.walk(scratch):
        for f in files:
            if f in ("stdout.txt", "stderr.txt"):
                path = os.path.join(root, f)
                print("----", path)
                with open(path, errors="replace") as fh:
                    print("".join(fh.readlines()[-40:]))
    sys.exit(1)


steps = []
if not wait_for([b"redits", b"Credits", b"Quit"], 60):
    fail("no title screen")
steps.append("title screen")
time.sleep(1.5)
send("n")
time.sleep(3.0)
# the dungeon's levels are still being made for a while after n, and Enter
# counts only once they are: press it until the first level shows
for _ in range(30):
    send("\r")
    if wait_for([b"Welcome to the lair", b"Welcome"], 4):
        break
else:
    fail("no first level after n, Enter")
steps.append("first level")
time.sleep(1.0)
send("r")
time.sleep(6.0)
if not alive():
    fail("quit while the robot played (exit code %s)" % code())
steps.append("robot played 6 s")
send("\x03")
end = time.time() + 20
while time.time() < end and alive():
    time.sleep(0.2)
if alive():
    fail("still running 20 s after Ctrl-C")
if code() != 0:
    fail("exit code %s after Ctrl-C" % code())
steps.append("Ctrl-C quit, exit 0")
with lock:
    restored = b"\x1b[?1049l" in seen
print("PASS:", ", ".join(steps), "; screen restored:", restored, "; state in", scratch)
