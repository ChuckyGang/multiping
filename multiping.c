/*
 * multiping - ping many hosts at once, one line per host, green = up, red = down.
 *
 * Single C file, no dependencies beyond the OS. Linux, macOS, Windows.
 *
 * Build:
 *   Linux/macOS:  cc -O2 -o multiping multiping.c -lpthread
 *   Windows:      x86_64-w64-mingw32-gcc -O2 -static -o multiping.exe multiping.c -liphlpapi -lws2_32
 *                 (MSVC: cl /O2 multiping.c iphlpapi.lib ws2_32.lib)
 *
 * How pings are sent (no admin/root needed in the normal case):
 *   Windows: IcmpSendEcho() from iphlpapi.
 *   Linux:   ICMP datagram socket (needs net.ipv4.ping_group_range to allow it,
 *            most distros do), else raw socket (root / cap_net_raw), else it
 *            runs the system "ping" command and parses the reply time.
 *   macOS:   ICMP datagram socket (always allowed), same fallbacks.
 *
 * Usage:
 *   multiping host1 host2 ...        ping these hosts (plus the hosts file)
 *   multiping 10.0.0.5=router        "host=label" shows a label next to it
 *   multiping -f hosts.txt           use a specific hosts file
 *   multiping                        only the default hosts file (~/.multiping_hosts)
 *
 * Keys: a=add host  d=delete host  r=reset stats  q=quit  Esc=cancel prompt
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>
#include <ctype.h>
#include <math.h>
#include <errno.h>

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  include <windows.h>
#  include <iphlpapi.h>
#  include <icmpapi.h>
#  include <conio.h>
#  include <process.h>
#  define strcasecmp _stricmp
typedef CRITICAL_SECTION mutex_t;
#  define mutex_init(m)   InitializeCriticalSection(m)
#  define mutex_lock(m)   EnterCriticalSection(m)
#  define mutex_unlock(m) LeaveCriticalSection(m)
#else
#  include <pthread.h>
#  include <strings.h>
#  include <unistd.h>
#  include <fcntl.h>
#  include <signal.h>
#  include <termios.h>
#  include <poll.h>
#  include <sys/ioctl.h>
#  include <sys/select.h>
#  include <sys/socket.h>
#  include <sys/wait.h>
#  include <netinet/in.h>
#  include <arpa/inet.h>
#  include <netdb.h>
typedef pthread_mutex_t mutex_t;
#  define mutex_init(m)   pthread_mutex_init(m, NULL)
#  define mutex_lock(m)   pthread_mutex_lock(m)
#  define mutex_unlock(m) pthread_mutex_unlock(m)
#endif

#define VERSION   "1.0"
#define MAX_HOSTS 256
#define RECENT    20
#define OUTBUF    (256 * 1024)

typedef struct host {
    char name[128];
    char label[64];
    volatile int removed;
    mutex_t lock;
    long sent, recv;
    double last, total, min, max;
    int has_last, has_minmax;
    double recent[RECENT];
    int nrecent, irecent;
    int fails;
    time_t fail_start, last_ok, down_since;
    int state;                      /* 0 unknown, 1 up, 2 down */
    unsigned short seq;
    unsigned int token;             /* identifies our packets in replies */
    struct sockaddr_in addr;
    int resolved;
    double next_resolve;
#ifdef _WIN32
    HANDLE icmp;
#else
    int sock;
#endif
} host_t;

static host_t *g_hosts[MAX_HOSTS];
static int g_nhosts;
static volatile int g_stop, g_beep;
static mutex_t g_loglock;

static struct {
    double interval, timeout;
    int down_after, beep, nosave, color;
    const char *file, *logfile;
    char filebuf[1024];
} cfg = { 1.0, 1.0, 1, 0, 0, 1, NULL, NULL, "" };

enum { MODE_ICMP_API, MODE_DGRAM, MODE_RAW, MODE_EXEC };
static int g_mode = MODE_EXEC;
static const char *mode_name[] = { "icmp", "icmp", "raw icmp", "via ping" };

/* ------------------------------------------------------------ portability */

static double now_s(void)
{
#ifdef _WIN32
    static LARGE_INTEGER freq;
    LARGE_INTEGER t;
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart / (double)freq.QuadPart;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
#endif
}

static void sleep_ms(int ms)
{
#ifdef _WIN32
    Sleep(ms);
#else
    struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
#endif
}

#ifdef _WIN32
static void start_thread(unsigned (__stdcall *fn)(void *), void *arg)
{
    HANDLE h = (HANDLE)_beginthreadex(NULL, 0, fn, arg, 0, NULL);
    if (h) CloseHandle(h);
}
#  define THREAD_FN(name) static unsigned __stdcall name(void *arg)
#  define THREAD_RET return 0
#else
static void start_thread(void *(*fn)(void *), void *arg)
{
    pthread_t t;
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
    pthread_create(&t, &a, fn, arg);
    pthread_attr_destroy(&a);
}
#  define THREAD_FN(name) static void *name(void *arg)
#  define THREAD_RET return NULL
#endif

/* ----------------------------------------------------------------- hosts */

static host_t *host_new(const char *name, const char *label)
{
    host_t *h = calloc(1, sizeof *h);
    if (!h) return NULL;
    snprintf(h->name, sizeof h->name, "%s", name);
    snprintf(h->label, sizeof h->label, "%s", label ? label : "");
    mutex_init(&h->lock);
    h->token = (unsigned)rand() ^ ((unsigned)(size_t)h << 4) ^ (unsigned)time(NULL);
#ifdef _WIN32
    h->icmp = INVALID_HANDLE_VALUE;
#else
    h->sock = -1;
#endif
    return h;
}

static void host_display(const host_t *h, char *out, size_t n)
{
    if (h->label[0]) snprintf(out, n, "%s (%s)", h->label, h->name);
    else snprintf(out, n, "%s", h->name);
}

static void host_reset(host_t *h)
{
    mutex_lock(&h->lock);
    h->sent = h->recv = 0;
    h->last = h->total = h->min = h->max = 0;
    h->has_last = h->has_minmax = 0;
    h->nrecent = h->irecent = 0;
    h->fails = 0;
    h->fail_start = h->last_ok = h->down_since = 0;
    h->state = 0;
    mutex_unlock(&h->lock);
}

/* returns 0 = nothing, 1 = went DOWN, 2 = came UP (*outage = seconds down) */
static int host_record(host_t *h, int ok, double rtt, double *outage)
{
    time_t now = time(NULL);
    int ev = 0;
    mutex_lock(&h->lock);
    h->sent++;
    if (!ok) {
        h->fails++;
        if (h->fails == 1) h->fail_start = now;
        h->has_last = 0;
        if (h->fails >= cfg.down_after && h->state != 2) {
            h->state = 2;
            h->down_since = h->fail_start;
            ev = 1;
        }
    } else {
        h->recv++;
        h->last = rtt;
        h->has_last = 1;
        h->total += rtt;
        h->recent[h->irecent] = rtt;
        h->irecent = (h->irecent + 1) % RECENT;
        if (h->nrecent < RECENT) h->nrecent++;
        if (!h->has_minmax || rtt < h->min) h->min = rtt;
        if (!h->has_minmax || rtt > h->max) h->max = rtt;
        h->has_minmax = 1;
        h->fails = 0;
        h->last_ok = now;
        if (h->state == 2) {
            *outage = difftime(now, h->down_since);
            h->down_since = 0;
            ev = 2;
        }
        h->state = 1;
    }
    mutex_unlock(&h->lock);
    return ev;
}

static int host_find(const char *key)
{
    int i;
    char *end;
    long n = strtol(key, &end, 10);
    if (*key && *end == '\0' && n >= 1 && n <= g_nhosts) return (int)n - 1;
    for (i = 0; i < g_nhosts; i++)
        if (!strcasecmp(g_hosts[i]->name, key) || (g_hosts[i]->label[0] && !strcasecmp(g_hosts[i]->label, key)))
            return i;
    return -1;
}

static int host_exists(const char *name)
{
    int i;
    for (i = 0; i < g_nhosts; i++)
        if (!strcasecmp(g_hosts[i]->name, name)) return 1;
    return 0;
}

/* ------------------------------------------------------------- hosts file */

static void load_hosts(const char *path)
{
    char line[512];
    FILE *f = fopen(path, "r");
    if (!f) return;
    while (fgets(line, sizeof line, f) && g_nhosts < MAX_HOSTS) {
        char *p = strchr(line, '#'), *name, *label, *e;
        if (p) *p = 0;
        p = line;
        while (isspace((unsigned char)*p)) p++;
        if (!*p) continue;
        name = p;
        while (*p && !isspace((unsigned char)*p)) p++;
        if (*p) *p++ = 0;
        while (isspace((unsigned char)*p)) p++;
        label = p;
        e = label + strlen(label);
        while (e > label && isspace((unsigned char)e[-1])) *--e = 0;
        if (!host_exists(name)) g_hosts[g_nhosts++] = host_new(name, label);
    }
    fclose(f);
}

static int save_hosts(const char *path)
{
    int i;
    FILE *f = fopen(path, "w");
    if (!f) return 0;
    fprintf(f, "# multiping hosts: one per line, optional label after the host\n");
    for (i = 0; i < g_nhosts; i++)
        fprintf(f, "%s%s%s\n", g_hosts[i]->name, g_hosts[i]->label[0] ? " " : "", g_hosts[i]->label);
    fclose(f);
    return 1;
}

/* ----------------------------------------------------------------- pinging */

static int resolve(host_t *h)
{
    struct addrinfo hints, *res = NULL;
    double now;
    if (h->resolved) return 1;
    now = now_s();
    if (now < h->next_resolve) return 0;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    if (getaddrinfo(h->name, NULL, &hints, &res) == 0 && res) {
        memcpy(&h->addr, res->ai_addr, sizeof h->addr);
        freeaddrinfo(res);
        h->resolved = 1;
        return 1;
    }
    if (res) freeaddrinfo(res);
    h->next_resolve = now + 30;         /* retry DNS every 30 s */
    return 0;
}

#ifdef _WIN32

static int ping_once(host_t *h, double *rtt)
{
    char sendbuf[32] = "multiping";
    char reply[sizeof(ICMP_ECHO_REPLY) + 32 + 8];
    PICMP_ECHO_REPLY er = (PICMP_ECHO_REPLY)reply;
    double t0, el;
    DWORD r;

    if (!resolve(h)) return 0;
    if (h->icmp == INVALID_HANDLE_VALUE) {
        h->icmp = IcmpCreateFile();
        if (h->icmp == INVALID_HANDLE_VALUE) return 0;
    }
    t0 = now_s();
    r = IcmpSendEcho(h->icmp, h->addr.sin_addr.s_addr, sendbuf, sizeof sendbuf, NULL,
                     reply, sizeof reply, (DWORD)(cfg.timeout * 1000));
    el = (now_s() - t0) * 1000.0;
    if (r == 0 || er->Status != IP_SUCCESS) return 0;
    *rtt = (double)er->RoundTripTime;
    if (*rtt == 0) *rtt = el < 1.0 ? el : 0.9;    /* API only has 1 ms resolution */
    return 1;
}

#else /* POSIX */

static unsigned short csum(const void *buf, int len)
{
    const unsigned char *p = buf;
    unsigned long sum = 0;
    while (len > 1) { sum += (p[0] << 8) | p[1]; p += 2; len -= 2; }
    if (len) sum += p[0] << 8;
    while (sum >> 16) sum = (sum & 0xffff) + (sum >> 16);
    return (unsigned short)~sum;
}

/* find "time=1.23 ms" / "tid<1ms" style value in ping output (any language) */
static int parse_ms(const char *s, double *rtt)
{
    const char *p = s;
    while ((p = strstr(p, "ms")) != NULL) {
        const char *q = p;
        int digits = 0;
        while (q > s && q[-1] == ' ') q--;
        while (q > s && (isdigit((unsigned char)q[-1]) || q[-1] == '.' || q[-1] == ',')) { q--; digits++; }
        if (digits) {
            const char *r = q;
            while (r > s && r[-1] == ' ') r--;
            if (r > s && (r[-1] == '=' || r[-1] == '<')) {
                char num[32];
                size_t n = (size_t)(p - q);
                if (n >= sizeof num) n = sizeof num - 1;
                memcpy(num, q, n);
                num[n] = 0;
                for (r = num; *r; r++) if (*r == ',') *(char *)r = '.';
                *rtt = atof(num);
                return 1;
            }
        }
        p += 2;
    }
    return 0;
}

static int ping_exec(host_t *h, double *rtt)
{
    int fds[2], status, n, len = 0;
    char buf[4096], ip[64], tsec[16], tms[16];
    pid_t pid;

    inet_ntop(AF_INET, &h->addr.sin_addr, ip, sizeof ip);
    snprintf(tsec, sizeof tsec, "%d", (int)ceil(cfg.timeout));
    snprintf(tms, sizeof tms, "%d", (int)(cfg.timeout * 1000));
    if (pipe(fds) < 0) return 0;
    pid = fork();
    if (pid < 0) { close(fds[0]); close(fds[1]); return 0; }
    if (pid == 0) {
        int dn = open("/dev/null", O_RDWR);
        dup2(fds[1], 1);
        if (dn >= 0) { dup2(dn, 0); dup2(dn, 2); }
        close(fds[0]);
        close(fds[1]);
#ifdef __APPLE__
        execlp("ping", "ping", "-n", "-c", "1", "-W", tms, "-t", tsec, ip, (char *)NULL);
#else
        execlp("ping", "ping", "-n", "-c", "1", "-W", tsec, ip, (char *)NULL);
#endif
        _exit(127);
    }
    close(fds[1]);
    while (len < (int)sizeof buf - 1 && (n = (int)read(fds[0], buf + len, sizeof buf - 1 - len)) > 0) len += n;
    buf[len] = 0;
    close(fds[0]);
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) ;
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) return 0;
    return parse_ms(buf, rtt);
}

static int ping_once(host_t *h, double *rtt)
{
    unsigned char pkt[64], buf[1500], *p;
    struct sockaddr_in from;
    socklen_t fl;
    double t0, deadline;
    int n;

    if (!resolve(h)) return 0;
    if (g_mode == MODE_EXEC) return ping_exec(h, rtt);
    if (h->sock < 0) {
        h->sock = socket(AF_INET, g_mode == MODE_DGRAM ? SOCK_DGRAM : SOCK_RAW, IPPROTO_ICMP);
        if (h->sock < 0) return 0;
    }
    memset(pkt, 0, sizeof pkt);
    pkt[0] = 8;                                   /* echo request */
    pkt[4] = (unsigned char)(getpid() >> 8);
    pkt[5] = (unsigned char)getpid();
    h->seq++;
    pkt[6] = (unsigned char)(h->seq >> 8);
    pkt[7] = (unsigned char)h->seq;
    memcpy(pkt + 8, &h->token, 4);                /* payload: token + seq, checked on reply */
    memcpy(pkt + 12, &h->seq, 2);
    memcpy(pkt + 14, "mp", 2);
    {
        unsigned short c = csum(pkt, sizeof pkt);
        pkt[2] = (unsigned char)(c >> 8);
        pkt[3] = (unsigned char)c;
    }
    t0 = now_s();
    if (sendto(h->sock, pkt, sizeof pkt, 0, (struct sockaddr *)&h->addr, sizeof h->addr) < 0) {
        close(h->sock);
        h->sock = -1;
        return 0;
    }
    deadline = t0 + cfg.timeout;
    for (;;) {
        struct pollfd pfd = { h->sock, POLLIN, 0 };
        double rem = deadline - now_s();
        if (rem <= 0) return 0;
        n = poll(&pfd, 1, (int)(rem * 1000) + 1);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return 0;
        fl = sizeof from;
        n = (int)recvfrom(h->sock, buf, sizeof buf, 0, (struct sockaddr *)&from, &fl);
        if (n <= 0) continue;
        p = buf;
        if ((p[0] >> 4) == 4) {                   /* raw socket / macOS: IP header first */
            int ihl = (p[0] & 0x0f) * 4;
            p += ihl;
            n -= ihl;
        }
        if (n < 16 || p[0] != 0) continue;        /* not an echo reply */
        if (memcmp(p + 8, pkt + 8, 8) != 0) continue;   /* not ours */
        *rtt = (now_s() - t0) * 1000.0;
        return 1;
    }
}

static void choose_mode(void)
{
    int s = socket(AF_INET, SOCK_DGRAM, IPPROTO_ICMP);
    if (s >= 0) { close(s); g_mode = MODE_DGRAM; return; }
    s = socket(AF_INET, SOCK_RAW, IPPROTO_ICMP);
    if (s >= 0) { close(s); g_mode = MODE_RAW; return; }
    g_mode = MODE_EXEC;
}

#endif /* POSIX */

/* -------------------------------------------------------------------- log */

static void fmt_dur(double secs, char *out, size_t n)
{
    long s = (long)secs;
    if (s < 60) snprintf(out, n, "%lds", s);
    else if (s < 3600) snprintf(out, n, "%ldm%02lds", s / 60, s % 60);
    else if (s < 86400) snprintf(out, n, "%ldh%02ldm", s / 3600, (s % 3600) / 60);
    else snprintf(out, n, "%ldd%02ldh", s / 86400, (s % 86400) / 3600);
}

static void log_event(host_t *h, int ev, double outage)
{
    char disp[200], when[32], dur[32];
    time_t t = time(NULL);
    FILE *f;
    if (!cfg.logfile) return;
    strftime(when, sizeof when, "%Y-%m-%d %H:%M:%S", localtime(&t));
    host_display(h, disp, sizeof disp);
    mutex_lock(&g_loglock);
    f = fopen(cfg.logfile, "a");
    if (f) {
        if (ev == 1) fprintf(f, "%s  %-32s DOWN\n", when, disp);
        else { fmt_dur(outage, dur, sizeof dur); fprintf(f, "%s  %-32s UP  (was down %s)\n", when, disp, dur); }
        fclose(f);
    }
    mutex_unlock(&g_loglock);
}

/* ----------------------------------------------------------------- worker */

THREAD_FN(worker)
{
    host_t *h = arg;
    while (!g_stop && !h->removed) {
        double t0 = now_s(), rtt = 0, outage = 0, wait;
        int ok = ping_once(h, &rtt);
        int ev;
        if (g_stop || h->removed) break;
        ev = host_record(h, ok, rtt, &outage);
        if (ev) {
            if (cfg.beep) g_beep = 1;
            log_event(h, ev, outage);
        }
        wait = cfg.interval - (now_s() - t0);
        while (wait > 0 && !g_stop && !h->removed) {
            sleep_ms(wait > 0.1 ? 100 : (int)(wait * 1000) + 1);
            wait = cfg.interval - (now_s() - t0);
        }
    }
#ifdef _WIN32
    if (h->icmp != INVALID_HANDLE_VALUE) IcmpCloseHandle(h->icmp);
#else
    if (h->sock >= 0) close(h->sock);
#endif
    THREAD_RET;
}

/* --------------------------------------------------------------- terminal */

#ifdef _WIN32
static DWORD g_oldmode;
static int g_haveoldmode;

static BOOL WINAPI ctrl_handler(DWORD type)
{
    (void)type;
    g_stop = 1;
    return TRUE;
}

static void term_init(void)
{
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode;
    if (GetConsoleMode(out, &mode)) {
        g_oldmode = mode;
        g_haveoldmode = 1;
        if (!SetConsoleMode(out, mode | 0x0004))      /* ENABLE_VIRTUAL_TERMINAL_PROCESSING */
            cfg.color = 0;
    }
    SetConsoleCtrlHandler(ctrl_handler, TRUE);
}

static void term_restore(void)
{
    if (g_haveoldmode) SetConsoleMode(GetStdHandle(STD_OUTPUT_HANDLE), g_oldmode);
}

static int term_getkey(void)
{
    int c;
    if (!_kbhit()) return -1;
    c = _getch();
    if (c == 0 || c == 0xE0) { if (_kbhit()) _getch(); return -1; }   /* arrows/function keys */
    return c;
}

static void term_size(int *cols, int *rows)
{
    CONSOLE_SCREEN_BUFFER_INFO i;
    *cols = 100; *rows = 30;
    if (GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &i)) {
        *cols = i.srWindow.Right - i.srWindow.Left + 1;
        *rows = i.srWindow.Bottom - i.srWindow.Top + 1;
    }
}

#else

static struct termios g_oldtio;
static int g_haveoldtio;

static void on_signal(int sig) { (void)sig; g_stop = 1; }

static void term_init(void)
{
    struct termios t;
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGHUP, on_signal);
    signal(SIGPIPE, SIG_IGN);
    if (!isatty(0)) return;
    if (tcgetattr(0, &g_oldtio) == 0) {
        g_haveoldtio = 1;
        t = g_oldtio;
        t.c_lflag &= ~(ICANON | ECHO);
        t.c_cc[VMIN] = 1;
        t.c_cc[VTIME] = 0;
        tcsetattr(0, TCSANOW, &t);
    }
}

static void term_restore(void)
{
    if (g_haveoldtio) tcsetattr(0, TCSANOW, &g_oldtio);
}

static int term_getkey(void)
{
    fd_set r;
    struct timeval tv = { 0, 0 };
    unsigned char c;
    if (!g_haveoldtio) return -1;
    FD_ZERO(&r);
    FD_SET(0, &r);
    if (select(1, &r, NULL, NULL, &tv) <= 0) return -1;
    if (read(0, &c, 1) != 1) return -1;
    if (c == 27) {                                  /* swallow escape sequences (arrows) */
        tv.tv_sec = 0; tv.tv_usec = 20000;
        FD_ZERO(&r); FD_SET(0, &r);
        if (select(1, &r, NULL, NULL, &tv) > 0) {
            unsigned char junk[16];
            if (read(0, junk, sizeof junk) > 0) return -1;
        }
    }
    return c;
}

static void term_size(int *cols, int *rows)
{
    struct winsize ws;
    *cols = 100; *rows = 30;
    if (ioctl(1, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0 && ws.ws_row > 0) {
        *cols = ws.ws_col;
        *rows = ws.ws_row;
    }
}
#endif

/* ----------------------------------------------------------------- render */

static char g_out[OUTBUF];
static size_t g_outlen;

static void app(const char *fmt, ...)
{
    va_list ap;
    int n;
    if (g_outlen >= OUTBUF - 1) return;
    va_start(ap, fmt);
    n = vsnprintf(g_out + g_outlen, OUTBUF - g_outlen, fmt, ap);
    va_end(ap);
    if (n > 0) g_outlen += (size_t)n < OUTBUF - g_outlen ? (size_t)n : OUTBUF - g_outlen - 1;
}

#define C(x) (cfg.color ? "\x1b[" x "m" : "")
#define EOL   (cfg.color ? "\x1b[K\n" : "\n")

static void fmt_ms(int has, double v, char *out, size_t n)
{
    if (!has) snprintf(out, n, "-");
    else if (v < 10) snprintf(out, n, "%.1f", v);
    else snprintf(out, n, "%.0f", v);
}

static void render(const char *prompt, const char *buf, const char *msg)
{
    int cols, rows, i, hw = 10, maxrows, nup = 0, ndown = 0;
    time_t now = time(NULL);
    char tm[16];

    term_size(&cols, &rows);
    g_outlen = 0;
    for (i = 0; i < g_nhosts; i++) {
        char d[200];
        int l;
        host_display(g_hosts[i], d, sizeof d);
        l = (int)strlen(d);
        if (l > hw) hw = l;
        if (g_hosts[i]->state == 1) nup++;
        else if (g_hosts[i]->state == 2) ndown++;
    }
    if (hw > cols - 78) hw = cols - 78 > 10 ? cols - 78 : 10;

    strftime(tm, sizeof tm, "%H:%M:%S", localtime(&now));
    if (cfg.color) app("\x1b[H");
    app("%smultiping%s  %d hosts   %s%d up%s   %s%d down%s   every %gs, timeout %gs, times in ms, %s   %s%s",
        C("1"), C("0"), g_nhosts, C("1;32"), nup, C("0"), ndown ? C("1;31") : "", ndown, C("0"),
        cfg.interval, cfg.timeout, mode_name[g_mode], tm, EOL);
    app("%s%3s %-*s %-11s %6s %6s %6s %6s %6s %6s %6s %5s %7s%s%s", C("2"), "#", hw, "HOST", "STATUS",
        "SENT", "RECV", "LOSS", "LAST", "AVG", "MIN", "MAX", "JTR", "LASTOK", C("0"), EOL);

    maxrows = rows - 4;
    if (maxrows < 1) maxrows = 1;
    for (i = 0; i < g_nhosts && i < maxrows; i++) {
        host_t *h = g_hosts[i];
        char name[200], status[48], loss[16], last[16], avg[16], mn[16], mx[16], jt[16], lastok[16], dur[32];
        const char *ncol, *scol;
        long sent, recv;
        double jitter = 0;
        int has_jitter = 0, state, k;

        mutex_lock(&h->lock);
        sent = h->sent; recv = h->recv; state = h->state;
        fmt_ms(h->has_last, h->last, last, sizeof last);
        fmt_ms(recv > 0, recv > 0 ? h->total / recv : 0, avg, sizeof avg);
        fmt_ms(h->has_minmax, h->min, mn, sizeof mn);
        fmt_ms(h->has_minmax, h->max, mx, sizeof mx);
        if (h->nrecent >= 2) {
            int first = (h->irecent - h->nrecent + RECENT) % RECENT;
            for (k = 1; k < h->nrecent; k++)
                jitter += fabs(h->recent[(first + k) % RECENT] - h->recent[(first + k - 1) % RECENT]);
            jitter /= h->nrecent - 1;
            has_jitter = 1;
        }
        if (h->last_ok == 0) snprintf(lastok, sizeof lastok, "never");
        else if (difftime(now, h->last_ok) < 2) snprintf(lastok, sizeof lastok, "now");
        else fmt_dur(difftime(now, h->last_ok), lastok, sizeof lastok);
        if (state == 2) { fmt_dur(difftime(now, h->down_since), dur, sizeof dur); snprintf(status, sizeof status, "DOWN %s", dur); }
        else if (state == 1) snprintf(status, sizeof status, " UP");
        else snprintf(status, sizeof status, " ...");
        mutex_unlock(&h->lock);

        fmt_ms(has_jitter, jitter, jt, sizeof jt);
        if (sent > 0) snprintf(loss, sizeof loss, "%.1f%%", 100.0 * (sent - recv) / sent);
        else snprintf(loss, sizeof loss, "-");
        host_display(h, name, sizeof name);
        if ((int)strlen(name) > hw) { name[hw - 1] = '~'; name[hw] = 0; }
        ncol = state == 2 ? C("1;31") : state == 1 ? C("32") : "";
        scol = state == 2 ? C("41;97;1") : state == 1 ? C("42;30;1") : C("2");
        app("%3d %s%-*s%s %s%-11s%s %6ld %6ld %6s %6s %6s %6s %6s %5s %7s%s",
            i + 1, ncol, hw, name, C("0"), scol, status, C("0"),
            sent, recv, loss, last, avg, mn, mx, jt, lastok, EOL);
    }
    if (g_nhosts > maxrows)
        app("%s... %d more hosts (enlarge the window)%s%s", C("2"), g_nhosts - maxrows, C("0"), EOL);
    app("%s", EOL);
    if (prompt) app("%s%s%s%s", C("1"), prompt, C("0"), buf);
    else app("%s[a]dd  [d]elete  [r]eset stats  [q]uit   %s%s", C("2"), C("0"), msg);
    if (cfg.color) app("\x1b[J%s", prompt ? "\x1b[?25h" : "\x1b[?25l");
    else app("\n");
    fwrite(g_out, 1, g_outlen, stdout);
    fflush(stdout);
}

/* ------------------------------------------------------------------- main */

static void usage(const char *argv0)
{
    fprintf(stderr,
        "multiping %s - ping many hosts at once, one line per host, green = up, red = down\n\n"
        "usage: %s [options] [host|host=label ...]\n\n"
        "  -f FILE   hosts file, one host per line with optional label (default ~/.multiping_hosts)\n"
        "  -i SEC    seconds between pings per host (default 1)\n"
        "  -t SEC    seconds to wait for a reply (default 1)\n"
        "  -d N      consecutive misses before a host shows DOWN (default 1)\n"
        "  -l FILE   append UP/DOWN changes with timestamps to FILE\n"
        "  -b        beep when a host goes down or comes back\n"
        "  -n        never rewrite the hosts file when adding/deleting hosts\n"
        "  -c        no colours\n"
        "  -h        this help\n\n"
        "keys while running: a=add host  d=delete host  r=reset stats  q=quit\n",
        VERSION, argv0);
}

static void default_file(void)
{
    const char *home = getenv("HOME");
#ifdef _WIN32
    if (!home || !*home) home = getenv("USERPROFILE");
#endif
    if (!home || !*home) home = ".";
    snprintf(cfg.filebuf, sizeof cfg.filebuf, "%s%c.multiping_hosts", home,
#ifdef _WIN32
             '\\'
#else
             '/'
#endif
            );
    cfg.file = cfg.filebuf;
}

int main(int argc, char **argv)
{
    int i, nargs = 0;
    char *args[MAX_HOSTS];
    char prompt[64] = "", buf[160] = "", msg[256] = "";
    int prompting = 0;
    double msg_until = 0, last_render = 0;

#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    g_mode = MODE_ICMP_API;
#endif
    srand((unsigned)time(NULL) ^ (unsigned)now_s());
    mutex_init(&g_loglock);
    default_file();

    for (i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (a[0] == '-' && a[1] && !a[2]) {
            switch (a[1]) {
            case 'f': if (++i < argc) cfg.file = argv[i]; break;
            case 'i': if (++i < argc) cfg.interval = atof(argv[i]); break;
            case 't': if (++i < argc) cfg.timeout = atof(argv[i]); break;
            case 'd': if (++i < argc) cfg.down_after = atoi(argv[i]); break;
            case 'l': if (++i < argc) cfg.logfile = argv[i]; break;
            case 'b': cfg.beep = 1; break;
            case 'n': cfg.nosave = 1; break;
            case 'c': cfg.color = 0; break;
            case 'h': usage(argv[0]); return 0;
            default: fprintf(stderr, "unknown option %s\n", a); usage(argv[0]); return 2;
            }
        } else if (a[0] == '-' && a[1] == '-') {
            if (!strcmp(a, "--help")) { usage(argv[0]); return 0; }
            fprintf(stderr, "unknown option %s\n", a); usage(argv[0]); return 2;
        } else if (nargs < MAX_HOSTS) {
            args[nargs++] = argv[i];
        }
    }
    if (cfg.interval < 0.1) cfg.interval = 0.1;
    if (cfg.timeout < 0.1) cfg.timeout = 0.1;
    if (cfg.down_after < 1) cfg.down_after = 1;

    load_hosts(cfg.file);
    for (i = 0; i < nargs; i++) {
        char *eq = strchr(args[i], '=');
        if (eq) *eq++ = 0;
        if (*args[i] && !host_exists(args[i]) && g_nhosts < MAX_HOSTS)
            g_hosts[g_nhosts++] = host_new(args[i], eq);
    }
    if (g_nhosts == 0) {
        usage(argv[0]);
        fprintf(stderr, "\nno hosts given and nothing in %s\n", cfg.file);
        return 2;
    }

#ifndef _WIN32
    choose_mode();
#endif
    for (i = 0; i < g_nhosts; i++) start_thread(worker, g_hosts[i]);

    term_init();
    if (cfg.color) { fputs("\x1b[?1049h\x1b[2J", stdout); fflush(stdout); }

    while (!g_stop) {
        int c, dirty = 0;
        while ((c = term_getkey()) >= 0) {
            dirty = 1;
            if (prompting) {
                if (c == '\r' || c == '\n') {
                    char *text = buf;
                    while (*text == ' ') text++;
                    if (prompt[0] == 'A' && *text) {
                        char *label = strchr(text, ' ');
                        if (label) { *label++ = 0; while (*label == ' ') label++; }
                        if (host_exists(text)) snprintf(msg, sizeof msg, "%s is already in the list", text);
                        else if (g_nhosts >= MAX_HOSTS) snprintf(msg, sizeof msg, "host list is full");
                        else {
                            host_t *h = host_new(text, label);
                            g_hosts[g_nhosts++] = h;
                            start_thread(worker, h);
                            if (!cfg.nosave) snprintf(msg, sizeof msg, save_hosts(cfg.file) ? "saved %s" : "could not write %s", cfg.file);
                        }
                        msg_until = now_s() + 5;
                    } else if (prompt[0] == 'D' && *text) {
                        int k = host_find(text);
                        if (k < 0) snprintf(msg, sizeof msg, "no such host: %s", text);
                        else {
                            g_hosts[k]->removed = 1;      /* thread exits by itself; struct is kept */
                            memmove(&g_hosts[k], &g_hosts[k + 1], (g_nhosts - k - 1) * sizeof g_hosts[0]);
                            g_nhosts--;
                            if (!cfg.nosave) snprintf(msg, sizeof msg, save_hosts(cfg.file) ? "saved %s" : "could not write %s", cfg.file);
                        }
                        msg_until = now_s() + 5;
                    }
                    prompting = 0;
                    buf[0] = 0;
                } else if (c == 27) {
                    prompting = 0;
                    buf[0] = 0;
                } else if (c == 127 || c == 8) {
                    size_t l = strlen(buf);
                    if (l) buf[l - 1] = 0;
                } else if (c >= 32 && c < 127) {
                    size_t l = strlen(buf);
                    if (l < sizeof buf - 1) { buf[l] = (char)c; buf[l + 1] = 0; }
                }
            } else if (c == 'q' || c == 'Q' || c == 3) {
                g_stop = 1;
            } else if (c == 'a' || c == 'A') {
                snprintf(prompt, sizeof prompt, "Add host [label]: ");
                prompting = 1;
                buf[0] = 0;
            } else if (c == 'd' || c == 'D') {
                snprintf(prompt, sizeof prompt, "Delete host (number or name): ");
                prompting = 1;
                buf[0] = 0;
            } else if (c == 'r' || c == 'R') {
                for (i = 0; i < g_nhosts; i++) host_reset(g_hosts[i]);
                snprintf(msg, sizeof msg, "statistics reset");
                msg_until = now_s() + 5;
            }
        }
        if (g_stop) break;
        if (msg[0] && now_s() > msg_until) msg[0] = 0;
        if (g_beep) { g_beep = 0; fputs("\a", stdout); fflush(stdout); }
        if (dirty || now_s() - last_render >= 0.25) {
            render(prompting ? prompt : NULL, buf, msg);
            last_render = now_s();
        }
        sleep_ms(30);
    }

    g_stop = 1;
    if (cfg.color) { fputs("\x1b[?25h\x1b[?1049l", stdout); fflush(stdout); }
    term_restore();
    sleep_ms(50);
    return 0;
}
