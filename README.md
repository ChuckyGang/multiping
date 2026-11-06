# multiping

Ping many hosts at once, one line per host. Green = answering, red = not.
One C file, no dependencies, one static binary per platform. Linux, macOS,
Windows, and a separate port for AmigaOS (68k). No admin/root needed in
normal use.

    multiping 192.168.10.1 pikvm=192.168.10.236 google.com
    multiping -f hosts.txt
    multiping                      # uses ~/.multiping_hosts

    multiping  3 hosts   2 up   1 down   every 1s, timeout 1s, times in ms, icmp   16:05:49
      # HOST                    STATUS        SENT   RECV   LOSS   LAST    AVG    MIN    MAX   JTR  LASTOK
      1 localhost (127.0.0.1)    UP              6      6   0.0%    0.1    0.0    0.0    0.1   0.0     now
      2 dead-box (10.255.254.1) DOWN 6s          6      0 100.0%      -      -      -      -     -   never
      3 cloudflare (1.1.1.1)     UP              6      6   0.0%    6.2    8.9    6.2     20   5.2     now

Keys: `a` add host, `d` delete host (number or name), `r` reset stats, `q` quit.
Adding/deleting rewrites the hosts file (`-n` to disable).

Hosts file: one host per line, optional label after it, `#` comments.

Options: `-i SEC` interval, `-t SEC` timeout, `-d N` misses before DOWN,
`-l FILE` log UP/DOWN changes with timestamps, `-b` beep on change, `-c` no colour.

## How pings are sent

| OS      | Method                                                                 |
|---------|------------------------------------------------------------------------|
| Windows | `IcmpSendEcho()` (iphlpapi), works as a normal user                    |
| Linux   | ICMP datagram socket if `net.ipv4.ping_group_range` allows it (most distros), else raw socket (root / `setcap cap_net_raw+ep multiping`), else it runs the system `ping` and parses the time |
| macOS   | ICMP datagram socket, always allowed                                   |
| AmigaOS | raw ICMP socket via `bsdsocket.library` (AmiTCP, Miami, Genesis, Roadshow), see below |

The title line shows which method is active (`icmp`, `raw icmp`, `via ping`).
IPv4 only for now.

## Downloads

Ready-built binaries, no install needed:

**https://github.com/ChuckyGang/multiping/releases/tag/latest**

On that page, scroll down to **Assets** and pick your file:

| File                     | For                                   |
|--------------------------|---------------------------------------|
| `multiping.exe`          | Windows 10/11, 64-bit                 |
| `multiping-linux-x86_64` | any 64-bit Linux (static, no libs)    |
| `multiping-macos-arm64`  | macOS on Apple Silicon                |
| `multiping-amiga`        | AmigaOS 2.0+, any 68k, needs a TCP/IP stack (see below) |

On Linux/macOS run `chmod +x` on the file first. Every push to `main`
rebuilds these, so "latest" is always the newest code.

## Build

    make                 # Linux / macOS
    make windows         # needs mingw-w64: x86_64-w64-mingw32-gcc, produces multiping.exe
    cl /O2 multiping.c iphlpapi.lib ws2_32.lib      # MSVC

    make amiga           # AmigaOS build, needs the Bartman toolchain (see amiga/Makefile)

`python/multiping.py` is the earlier Python version, same screen and keys.

## Amiga version

`amiga/` holds a separate port for AmigaOS 2.0+ on any 68000 or better. It
needs a running TCP/IP stack with `bsdsocket.library` that allows raw ICMP
sockets: AmiTCP, Miami, Genesis and Roadshow all do. Same screen, same
keys, same hosts-file format, in the Shell window (or its own console
window when started from Workbench).

    multiping 192.168.1.1 fileserver=192.168.1.10 amiga.org
    multiping FILE=work:hosts.txt INTERVAL=2 TIMEOUT=1500 LOG=ram:ping.log

    HOSTS/M,FILE/K,INTERVAL=I/K/N,TIMEOUT=T/K/N,DOWNAFTER=D/K/N,LOG/K,BEEP/S,NOSAVE/S,NOCOLOR/S

Tested on real hardware is still pending; verified in Amiberry on WB3.2.3/68060
with its bsdsocket emulation (note: Amiberry as Flatpak or as a normal user
cannot open raw sockets, so there it exits with errno 1).

Differences from the other builds: the hosts file defaults to
`S:multiping_hosts`, INTERVAL is in seconds and TIMEOUT in milliseconds,
and UP/DOWN are shown as pen 3 / inverse video rather than green / red,
because a stock 4-colour Workbench has neither. Built with the
Bartman/Abyss ELF toolchain, no C library, 23 KB executable.
