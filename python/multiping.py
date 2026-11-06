#!/usr/bin/env python3
"""multiping - ping many hosts at once, one line per host, green = up, red = down.

Single file, Python 3.7+, standard library only. Uses the system "ping"
command, so no root/admin is needed. Works on Linux, macOS and Windows
(Windows 10+ console, Windows Terminal, PowerShell, cmd).

Usage:
  multiping.py host1 host2 ...       ping these hosts (plus the hosts file)
  multiping.py 10.0.0.5=router ...   "host=label" shows a label next to it
  multiping.py -f hosts.txt          use a specific hosts file
  multiping.py                       only the default hosts file

Hosts file (default ~/.multiping_hosts): one host per line, optional label:
  192.168.10.236  pikvm-a4000t
  # lines starting with # are comments

Keys while running:
  a  add a host        d  delete a host        r  reset statistics
  q  quit              Esc cancels a prompt

Adding/deleting a host rewrites the hosts file unless --no-save is given.
"""

import argparse
import math
import os
import platform
import re
import shutil
import subprocess
import sys
import threading
import time
from collections import deque
from datetime import datetime

IS_WIN = platform.system() == "Windows"
IS_MAC = platform.system() == "Darwin"
DEFAULT_FILE = os.path.join(os.path.expanduser("~"), ".multiping_hosts")

# Matches the reply time in any language/locale:
#   "time=0.345 ms"  "time<1ms"  "tid=12ms"  "Zeit=3ms"  "temps=4 ms"
RTT_RE = re.compile(r"[=<]\s*(\d+(?:[.,]\d+)?)\s*ms", re.IGNORECASE)

STOP = threading.Event()
BEEP = threading.Event()
LOG_LOCK = threading.Lock()

# ---------------------------------------------------------------- terminal --

ESC = "\x1b["
COLOR = True


def c(code):
    return ESC + code + "m" if COLOR else ""


def enable_vt():
    """Turn on ANSI escape processing. Returns False on consoles without it."""
    global COLOR
    if not IS_WIN:
        return True
    try:
        import ctypes
        k32 = ctypes.windll.kernel32
        h = k32.GetStdHandle(-11)  # STD_OUTPUT_HANDLE
        mode = ctypes.c_uint32()
        if k32.GetConsoleMode(h, ctypes.byref(mode)):
            if k32.SetConsoleMode(h, mode.value | 0x0004):  # ENABLE_VIRTUAL_TERMINAL_PROCESSING
                return True
    except Exception:
        pass
    COLOR = False
    return False


class Keyboard:
    """Non-blocking single-key reader for Windows and POSIX."""

    def __init__(self):
        self.enabled = sys.stdin.isatty()
        self.old = None
        if self.enabled and not IS_WIN:
            import termios
            import tty
            self.fd = sys.stdin.fileno()
            self.old = termios.tcgetattr(self.fd)
            tty.setcbreak(self.fd)

    def restore(self):
        if self.old is not None:
            import termios
            termios.tcsetattr(self.fd, termios.TCSADRAIN, self.old)

    def getkey(self):
        if not self.enabled:
            return None
        if IS_WIN:
            import msvcrt
            if msvcrt.kbhit():
                ch = msvcrt.getwch()
                if ch in ("\x00", "\xe0"):  # function/arrow key prefix
                    msvcrt.getwch()
                    return None
                return ch
            return None
        import select
        if select.select([sys.stdin], [], [], 0)[0]:
            return sys.stdin.read(1)
        return None


# -------------------------------------------------------------------- ping --

def ping_cmd(host, timeout):
    if IS_WIN:
        return ["ping", "-n", "1", "-w", str(int(timeout * 1000)), host]
    if IS_MAC:
        return ["ping", "-n", "-c", "1", "-W", str(int(timeout * 1000)),
                "-t", str(max(1, math.ceil(timeout))), host]
    w = str(int(timeout)) if float(timeout).is_integer() else str(timeout)
    return ["ping", "-n", "-c", "1", "-W", w, host]


POPEN_KW = {}
if IS_WIN:
    POPEN_KW["creationflags"] = getattr(subprocess, "CREATE_NO_WINDOW", 0)


def ping_once(host, timeout):
    """Return the round-trip time in ms, or None if there was no reply."""
    try:
        p = subprocess.run(ping_cmd(host, timeout), stdout=subprocess.PIPE,
                           stderr=subprocess.DEVNULL, universal_newlines=True,
                           errors="replace", timeout=timeout + 3, **POPEN_KW)
    except (subprocess.TimeoutExpired, OSError):
        return None
    m = RTT_RE.search(p.stdout)
    if not m:
        return None
    if not IS_WIN and p.returncode != 0:
        return None
    return float(m.group(1).replace(",", "."))


# -------------------------------------------------------------------- host --

class Host:
    def __init__(self, name, label=""):
        self.name = name
        self.label = label
        self.removed = False
        self.lock = threading.Lock()
        self.reset()

    def display(self):
        return "%s (%s)" % (self.label, self.name) if self.label else self.name

    def reset(self):
        with self.lock:
            self.sent = 0
            self.recv = 0
            self.last = None
            self.total = 0.0
            self.min = None
            self.max = None
            self.recent = deque(maxlen=20)   # recent RTTs, for jitter
            self.fails = 0                   # consecutive misses
            self.fail_start = None
            self.last_ok = None
            self.down_since = None
            self.state = None                # None=unknown, True=up, False=down

    def record(self, rtt, down_after):
        """Store one ping result. Returns ("DOWN", None), ("UP", outage_s) or None."""
        now = time.time()
        with self.lock:
            self.sent += 1
            if rtt is None:
                self.fails += 1
                if self.fails == 1:
                    self.fail_start = now
                self.last = None
                if self.fails >= down_after and self.state is not False:
                    self.state = False
                    self.down_since = self.fail_start
                    return ("DOWN", None)
                return None
            self.recv += 1
            self.last = rtt
            self.total += rtt
            self.recent.append(rtt)
            self.min = rtt if self.min is None else min(self.min, rtt)
            self.max = rtt if self.max is None else max(self.max, rtt)
            self.fails = 0
            self.last_ok = now
            if self.state is False:
                outage = now - self.down_since
                self.state = True
                self.down_since = None
                return ("UP", outage)
            self.state = True
            return None

    def snapshot(self):
        with self.lock:
            jitter = None
            if len(self.recent) >= 2:
                r = list(self.recent)
                jitter = sum(abs(a - b) for a, b in zip(r, r[1:])) / (len(r) - 1)
            return dict(name=self.display(), state=self.state, sent=self.sent,
                        recv=self.recv, last=self.last, min=self.min, max=self.max,
                        avg=(self.total / self.recv) if self.recv else None,
                        jitter=jitter, last_ok=self.last_ok, down_since=self.down_since)


def worker(host, args):
    while not STOP.is_set() and not host.removed:
        t0 = time.monotonic()
        ev = host.record(ping_once(host.name, args.timeout), args.down_after)
        if ev:
            if args.beep:
                BEEP.set()
            if args.log:
                log_event(args.log, host, ev[0], ev[1])
        STOP.wait(max(0.0, args.interval - (time.monotonic() - t0)))


def start_host(host, args):
    threading.Thread(target=worker, args=(host, args), daemon=True).start()


def log_event(path, host, what, outage):
    line = "%s  %-32s %s" % (datetime.now().strftime("%Y-%m-%d %H:%M:%S"), host.display(), what)
    if outage is not None:
        line += "  (was down %s)" % fmt_dur(outage)
    with LOG_LOCK:
        try:
            with open(path, "a") as f:
                f.write(line + "\n")
        except OSError:
            pass


# -------------------------------------------------------------- hosts file --

def load_hosts(path):
    hosts = []
    try:
        with open(path) as f:
            lines = f.read().splitlines()
    except OSError:
        return hosts
    for ln in lines:
        ln = ln.split("#", 1)[0].strip()
        if not ln:
            continue
        parts = ln.split(None, 1)
        hosts.append(Host(parts[0], parts[1].strip() if len(parts) > 1 else ""))
    return hosts


def save_hosts(path, hosts):
    try:
        with open(path, "w") as f:
            f.write("# multiping hosts: one per line, optional label after the host\n")
            for h in hosts:
                f.write(("%s %s" % (h.name, h.label)).rstrip() + "\n")
        return True
    except OSError:
        return False


# ------------------------------------------------------------------ render --

def fmt_ms(v):
    if v is None:
        return "-"
    if v < 10:
        return "%.1f" % v
    return "%.0f" % v


def fmt_dur(sec):
    sec = int(sec)
    if sec < 60:
        return "%ds" % sec
    if sec < 3600:
        return "%dm%02ds" % (sec // 60, sec % 60)
    if sec < 86400:
        return "%dh%02dm" % (sec // 3600, (sec % 3600) // 60)
    return "%dd%02dh" % (sec // 86400, (sec % 86400) // 3600)


def render(hosts, args, prompt, buf, msg):
    cols, rows = shutil.get_terminal_size((100, 30))
    now = time.time()
    snaps = [h.snapshot() for h in hosts]
    nup = sum(1 for s in snaps if s["state"] is True)
    ndown = sum(1 for s in snaps if s["state"] is False)
    hw = max([len(s["name"]) for s in snaps] + [10])
    hw = min(hw, max(10, cols - 78))

    title = ("%smultiping%s  %d hosts   %s%d up%s   %s%d down%s   every %gs, timeout %gs, "
             "times in ms   %s" % (c("1"), c("0"), len(snaps),
                                   c("1;32"), nup, c("0"),
                                   c("1;31") if ndown else "", ndown, c("0"),
                                   args.interval, args.timeout,
                                   datetime.now().strftime("%H:%M:%S")))
    header = "%s%3s %-*s %-11s %6s %6s %6s %6s %6s %6s %6s %5s %7s%s" % (
        c("2"), "#", hw, "HOST", "STATUS", "SENT", "RECV", "LOSS",
        "LAST", "AVG", "MIN", "MAX", "JTR", "LASTOK", c("0"))
    lines = [title, header]

    maxrows = max(1, rows - 4)
    for i, s in enumerate(snaps[:maxrows], 1):
        name = s["name"]
        if len(name) > hw:
            name = name[:hw - 1] + "~"
        if s["state"] is False:
            status = c("41;97;1") + "%-11s" % ("DOWN " + fmt_dur(now - s["down_since"])) + c("0")
            name = c("1;31") + "%-*s" % (hw, name) + c("0")
        elif s["state"] is True:
            status = c("42;30;1") + "%-11s" % " UP" + c("0")
            name = c("32") + "%-*s" % (hw, name) + c("0")
        else:
            status = c("2") + "%-11s" % " ..." + c("0")
            name = "%-*s" % (hw, name)
        loss = "%.1f%%" % (100.0 * (s["sent"] - s["recv"]) / s["sent"]) if s["sent"] else "-"
        if s["last_ok"] is None:
            lastok = "never"
        elif now - s["last_ok"] < 2:
            lastok = "now"
        else:
            lastok = fmt_dur(now - s["last_ok"])
        lines.append("%3d %s %s %6d %6d %6s %6s %6s %6s %6s %5s %7s" % (
            i, name, status, s["sent"], s["recv"], loss, fmt_ms(s["last"]),
            fmt_ms(s["avg"]), fmt_ms(s["min"]), fmt_ms(s["max"]),
            fmt_ms(s["jitter"]), lastok))
    if len(snaps) > maxrows:
        lines.append(c("2") + "... %d more hosts (enlarge the window)" % (len(snaps) - maxrows) + c("0"))

    lines.append("")
    if prompt:
        lines.append(c("1") + prompt + c("0") + buf)
    else:
        lines.append(c("2") + "[a]dd  [d]elete  [r]eset stats  [q]uit   " + c("0") + msg)

    if COLOR:
        out = ESC + "H" + (ESC + "K\n").join(lines) + ESC + "J"
        out += ESC + "?25h" if prompt else ESC + "?25l"
    else:
        os.system("cls" if IS_WIN else "clear")
        out = "\n".join(lines)
    sys.stdout.write(out)
    sys.stdout.flush()


# -------------------------------------------------------------------- main --

def find_host(hosts, key):
    key = key.strip()
    if key.isdigit() and 1 <= int(key) <= len(hosts):
        return hosts[int(key) - 1]
    for h in hosts:
        if key.lower() in (h.name.lower(), h.label.lower()):
            return h
    return None


def main():
    ap = argparse.ArgumentParser(
        description="Ping many hosts at once, one line per host, green = up, red = down.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="keys while running: a=add host, d=delete host, r=reset stats, q=quit")
    ap.add_argument("hosts", nargs="*", help="hosts to ping (host, or host=label)")
    ap.add_argument("-f", "--file", default=DEFAULT_FILE, metavar="FILE",
                    help="hosts file, one host per line (default: %s)" % DEFAULT_FILE)
    ap.add_argument("-i", "--interval", type=float, default=1.0, metavar="SEC",
                    help="seconds between pings per host (default 1)")
    ap.add_argument("-t", "--timeout", type=float, default=1.0, metavar="SEC",
                    help="seconds to wait for a reply (default 1)")
    ap.add_argument("-d", "--down-after", type=int, default=1, metavar="N",
                    help="consecutive misses before a host shows DOWN (default 1)")
    ap.add_argument("-l", "--log", metavar="FILE", help="append UP/DOWN changes with timestamps to FILE")
    ap.add_argument("-b", "--beep", action="store_true", help="beep when a host goes down or comes back")
    ap.add_argument("--no-save", action="store_true", help="never rewrite the hosts file")
    ap.add_argument("--no-color", action="store_true", help="plain text, no colours")
    args = ap.parse_args()

    if shutil.which("ping") is None:
        sys.exit("multiping: no 'ping' command found on this system")

    hosts = load_hosts(args.file)
    for spec in args.hosts:
        name, _, label = spec.partition("=")
        if name and not any(h.name == name for h in hosts):
            hosts.append(Host(name, label))
    if not hosts:
        ap.error("no hosts given and nothing in %s" % args.file)

    global COLOR
    if args.no_color:
        COLOR = False
    enable_vt()
    kb = Keyboard()
    for h in hosts:
        start_host(h, args)

    prompt = None
    buf = ""
    msg = ""
    msg_until = 0.0

    def say(text):
        nonlocal msg, msg_until
        msg, msg_until = text, time.time() + 5

    def save():
        if args.no_save:
            return
        say("saved %s" % args.file if save_hosts(args.file, hosts) else "could not write %s" % args.file)

    if COLOR:
        sys.stdout.write(ESC + "?1049h" + ESC + "2J")
    try:
        while True:
            key = kb.getkey()
            while key is not None:
                if prompt:
                    if key in ("\r", "\n"):
                        text, buf = buf.strip(), ""
                        if prompt.startswith("Add") and text:
                            parts = text.split(None, 1)
                            name, label = parts[0], (parts[1] if len(parts) > 1 else "")
                            if any(h.name == name for h in hosts):
                                say("%s is already in the list" % name)
                            else:
                                h = Host(name, label)
                                hosts.append(h)
                                start_host(h, args)
                                save()
                        elif prompt.startswith("Delete") and text:
                            h = find_host(hosts, text)
                            if h is None:
                                say("no such host: %s" % text)
                            else:
                                h.removed = True
                                hosts.remove(h)
                                save()
                        prompt = None
                    elif key == "\x1b":
                        prompt, buf = None, ""
                    elif key in ("\x7f", "\x08"):
                        buf = buf[:-1]
                    elif key.isprintable():
                        buf += key
                elif key in ("q", "Q", "\x03"):
                    return
                elif key in ("a", "A"):
                    prompt, buf = "Add host [label]: ", ""
                elif key in ("d", "D"):
                    prompt, buf = "Delete host (number or name): ", ""
                elif key in ("r", "R"):
                    for h in hosts:
                        h.reset()
                    say("statistics reset")
                key = kb.getkey()
            if time.time() > msg_until:
                msg = ""
            if BEEP.is_set():
                BEEP.clear()
                sys.stdout.write("\a")
            render(hosts, args, prompt, buf, msg)
            time.sleep(0.25)
    except KeyboardInterrupt:
        pass
    finally:
        STOP.set()
        if COLOR:
            sys.stdout.write(ESC + "?25h" + ESC + "?1049l")
        sys.stdout.flush()
        kb.restore()


if __name__ == "__main__":
    main()
