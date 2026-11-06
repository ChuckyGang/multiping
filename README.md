# multiping

Ping many hosts at once, one line per host. Green = answering, red = not.
One C file, no dependencies, one static binary per platform. Linux, macOS,
Windows. No admin/root needed in normal use.

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

The title line shows which method is active (`icmp`, `raw icmp`, `via ping`).
IPv4 only for now.

## Downloads

Every push builds Linux (static x86_64), Windows (x64 .exe) and macOS (arm64)
binaries. Grab them from the **latest** release on GitHub.

## Build

    make                 # Linux / macOS
    make windows         # needs mingw-w64: x86_64-w64-mingw32-gcc, produces multiping.exe
    cl /O2 multiping.c iphlpapi.lib ws2_32.lib      # MSVC

`python/multiping.py` is the earlier Python version, same screen and keys.
