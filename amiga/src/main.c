/*
 * multiping for AmigaOS - ping many hosts at once, one line per host.
 *
 * AmigaOS 2.0+ (dos.library 37), any 68000, and a TCP/IP stack that
 * provides bsdsocket.library with raw ICMP sockets: AmiTCP, Miami,
 * Genesis, Roadshow. Runs in the Shell window using the console's own
 * ANSI sequences; from Workbench it opens a console window.
 *
 * Built with the Bartman/Abyss ELF toolchain, -nostdlib: no C library,
 * everything here is AmigaOS calls plus the few helpers in support/.
 *
 * Usage:
 *   multiping HOSTS/M,FILE/K,INTERVAL=I/K/N,TIMEOUT=T/K/N,DOWNAFTER=D/K/N,
 *             LOG/K,BEEP/S,NOSAVE/S,NOCOLOR/S
 *
 *   multiping 192.168.1.1 fileserver=192.168.1.10 amiga.org
 *   multiping FILE=work:hosts.txt INTERVAL=2 TIMEOUT=1500
 *
 * Hosts file (default S:multiping_hosts): one host per line, optional
 * label after it, # comments. Keys: a=add d=delete r=reset q=quit.
 *
 * Status shown as UP (pen 3) or DOWN (inverse video) so it is readable on
 * any Workbench palette: a stock 4-colour screen has no red or green.
 */

#include <exec/types.h>
#include <exec/execbase.h>
#include <exec/memory.h>
#include <exec/io.h>
#include <exec/ports.h>
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <dos/datetime.h>
#include <dos/rdargs.h>
#include <devices/timer.h>
#include <workbench/startup.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/timer.h>
#include "bsdsocket_lp.h"
#include "../support/gcc8_c_support.h"

#define VERSION    "1.0"
#define MAX_HOSTS  64
#define RECENT     20
#define OUTBUF     32768
#define CSI        "\x9b"

struct ExecBase *SysBase;
struct DosLibrary *DOSBase;
struct Library *SocketBase;
struct Device *TimerBase;
struct WBStartup *WBStartupMsg;        /* set by support/_start on Workbench launch */

typedef struct host {
    char  name[64];
    char  label[32];
    UWORD uid;                         /* identifies this host in ICMP payloads */
    ULONG addr;                        /* network byte order, 0 = unresolved */
    ULONG next_resolve;                /* ms clock */
    UWORD seq;
    int   outstanding;                 /* request sent, no reply yet */
    struct timeval t_sent;
    ULONG next_send;                   /* ms clock */
    ULONG sent, recv;
    ULONG last_us, min_us, max_us;
    int   has_last, has_minmax;
    ULONG avg_total, avg_n;            /* avg = avg_total / avg_n (us) */
    ULONG recent[RECENT];
    int   nrecent, irecent;
    ULONG fails;
    ULONG fail_start, last_ok, down_since;   /* seconds (GetSysTime) */
    int   state;                       /* 0 unknown, 1 up, 2 down */
} host_t;

static host_t g_hosts[MAX_HOSTS];
static int g_nhosts;
static UWORD g_next_uid = 1;

static struct {
    ULONG interval_ms, timeout_ms;
    LONG  down_after;
    int   beep, nosave, color;
    const char *file, *logfile;
} cfg = { 1000, 1000, 1, 0, 0, 1, "S:multiping_hosts", NULL };

static BPTR g_in, g_out, g_con;
#ifdef DEBUG_TRACE
static int g_verbose = 1;
#else
static int g_verbose;
#endif

#ifdef DEBUG_TRACE
/* debug build: every trace line goes to DH9:trace.txt, opened and closed per line */
static void dbg_line(const char *msg)
{
    BPTR f = Open("DH9:trace.txt", MODE_READWRITE);
    if (!f) return;
    Seek(f, 0, OFFSET_END);
    Write(f, (APTR)msg, (LONG)strlen(msg));
    Write(f, "\n", 1);
    Close(f);
}
#define TRACE(msg) dbg_line("multiping: " msg)
#else
#define TRACE(msg) do { if (g_verbose) con_write("multiping: " msg "\n"); } while (0)
#endif
static LONG g_sock = -1;
static UWORD g_icmp_id;
static ULONG g_base_secs;
static int g_rows = 25, g_cols = 80;
static struct MsgPort *g_tport;
static struct timerequest *g_treq;

/* ------------------------------------------------------------ strings */

static void str_copy(char *dst, const char *src, ULONG n)
{
    ULONG i = 0;
    while (i + 1 < n && src[i]) { dst[i] = src[i]; i++; }
    dst[i] = 0;
}

static void str_cat(char *dst, const char *src)
{
    while (*dst) dst++;
    while (*src) *dst++ = *src++;
    *dst = 0;
}

static int str_ieq(const char *a, const char *b)
{
    for (;; a++, b++) {
        int ca = *a, cb = *b;
        if (ca >= 'A' && ca <= 'Z') ca += 32;
        if (cb >= 'A' && cb <= 'Z') cb += 32;
        if (ca != cb) return 0;
        if (!ca) return 1;
    }
}

static int is_space(int c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

static ULONG str_u(const char *s, int *ok)
{
    ULONG v = 0;
    *ok = 0;
    while (*s >= '0' && *s <= '9') { v = v * 10 + (ULONG)(*s - '0'); s++; *ok = 1; }
    if (*s) *ok = 0;
    return v;
}

static void fmt_u(char *dst, ULONG v)
{
    char tmp[12];
    int n = 0;
    do { tmp[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (n) *dst++ = tmp[--n];
    *dst = 0;
}

static void fmt_u2(char *dst, ULONG v)          /* two digits, zero padded */
{
    dst[0] = (char)('0' + (v / 10) % 10);
    dst[1] = (char)('0' + v % 10);
    dst[2] = 0;
}

static void fmt_ms(char *dst, int has, ULONG us)
{
    if (!has) { dst[0] = '-'; dst[1] = 0; return; }
    if (us < 10000) {                                /* < 10 ms: one decimal */
        ULONG t = (us + 50) / 100;
        fmt_u(dst, t / 10);
        while (*dst) dst++;
        dst[0] = '.';
        dst[1] = (char)('0' + t % 10);
        dst[2] = 0;
    } else {
        fmt_u(dst, (us + 500) / 1000);
    }
}

static void fmt_dur(char *dst, ULONG s)
{
    char t[8];
    if (s < 60) { fmt_u(dst, s); str_cat(dst, "s"); }
    else if (s < 3600) { fmt_u(dst, s / 60); str_cat(dst, "m"); fmt_u2(t, s % 60); str_cat(dst, t); str_cat(dst, "s"); }
    else if (s < 86400) { fmt_u(dst, s / 3600); str_cat(dst, "h"); fmt_u2(t, (s % 3600) / 60); str_cat(dst, t); str_cat(dst, "m"); }
    else { fmt_u(dst, s / 86400); str_cat(dst, "d"); fmt_u2(t, (s % 86400) / 3600); str_cat(dst, t); str_cat(dst, "h"); }
}

/* ------------------------------------------------------------- output */

static char g_obuf[OUTBUF];
static ULONG g_olen;

static void o_str(const char *s)
{
    while (*s && g_olen < OUTBUF - 1) g_obuf[g_olen++] = *s++;
}

static void o_col(const char *s, int width, int left)   /* pad/truncate to width */
{
    int len = (int)strlen(s), i;
    if (len > width) len = width;
    if (!left) for (i = len; i < width; i++) o_str(" ");
    for (i = 0; i < len && g_olen < OUTBUF - 1; i++) g_obuf[g_olen++] = s[i];
    if (left) for (i = len; i < width; i++) o_str(" ");
}

static void o_flush(void)
{
    if (g_olen) Write(g_out, g_obuf, (LONG)g_olen);
    g_olen = 0;
}

static void o_sgr(const char *code)     /* CSI code m, only when colour is on */
{
    if (!cfg.color) return;
    o_str(CSI);
    o_str(code);
    o_str("m");
}

static void con_write(const char *s) { Write(g_out, (APTR)s, (LONG)strlen(s)); }

/* --------------------------------------------------------------- time */

static ULONG now_ms(void)
{
    struct timeval tv;
    GetSysTime(&tv);
    return (tv.tv_secs - g_base_secs) * 1000 + tv.tv_micro / 1000;
}

static ULONG now_secs(void)
{
    struct timeval tv;
    GetSysTime(&tv);
    return tv.tv_secs;
}

static LONG tv_diff_us(const struct timeval *a, const struct timeval *b)
{
    return (LONG)(a->tv_secs - b->tv_secs) * 1000000L + ((LONG)a->tv_micro - (LONG)b->tv_micro);
}

static void clock_str(char *date /* 16 or NULL */, char *tm /* 16 */)
{
    struct DateTime dt;
    char ds[LEN_DATSTRING], ts[LEN_DATSTRING];
    DateStamp(&dt.dat_Stamp);
    dt.dat_Format = FORMAT_INT;
    dt.dat_Flags = 0;
    dt.dat_StrDay = NULL;
    dt.dat_StrDate = ds;
    dt.dat_StrTime = ts;
    ds[0] = ts[0] = 0;
    DateToStr(&dt);
    if (date) str_copy(date, ds, 16);
    str_copy(tm, ts, 16);
}

/* -------------------------------------------------------------- hosts */

static void host_reset(host_t *h)
{
    h->sent = h->recv = 0;
    h->last_us = h->min_us = h->max_us = 0;
    h->has_last = h->has_minmax = 0;
    h->avg_total = h->avg_n = 0;
    h->nrecent = h->irecent = 0;
    h->fails = 0;
    h->fail_start = h->last_ok = h->down_since = 0;
    h->state = 0;
}

static host_t *host_add(const char *name, const char *label)
{
    host_t *h;
    if (g_nhosts >= MAX_HOSTS) return NULL;
    h = &g_hosts[g_nhosts++];
    memset(h, 0, sizeof *h);
    str_copy(h->name, name, sizeof h->name);
    str_copy(h->label, label ? label : "", sizeof h->label);
    h->uid = g_next_uid++;
    return h;
}

static int host_exists(const char *name)
{
    int i;
    for (i = 0; i < g_nhosts; i++) if (str_ieq(g_hosts[i].name, name)) return 1;
    return 0;
}

static int host_find(const char *key)
{
    int i, ok;
    ULONG n = str_u(key, &ok);
    if (ok && n >= 1 && n <= (ULONG)g_nhosts) return (int)n - 1;
    for (i = 0; i < g_nhosts; i++)
        if (str_ieq(g_hosts[i].name, key) || (g_hosts[i].label[0] && str_ieq(g_hosts[i].label, key)))
            return i;
    return -1;
}

static host_t *host_by_uid(UWORD uid)
{
    int i;
    for (i = 0; i < g_nhosts; i++) if (g_hosts[i].uid == uid) return &g_hosts[i];
    return NULL;
}

static void host_display(const host_t *h, char *out)     /* out >= 100 */
{
    if (h->label[0]) { str_copy(out, h->label, 32); str_cat(out, " ("); str_cat(out, h->name); str_cat(out, ")"); }
    else str_copy(out, h->name, 64);
}

/* --------------------------------------------------------------- log */

static void log_event(host_t *h, int ev, ULONG outage)
{
    char line[200], date[16], tm[16], disp[100], dur[32];
    BPTR f;
    int i;
    if (!cfg.logfile) return;
    clock_str(date, tm);
    host_display(h, disp);
    str_copy(line, date, sizeof line);
    str_cat(line, " ");
    str_cat(line, tm);
    str_cat(line, "  ");
    str_cat(line, disp);
    for (i = (int)strlen(disp); i < 32; i++) str_cat(line, " ");
    if (ev == 1) str_cat(line, " DOWN\n");
    else { fmt_dur(dur, outage); str_cat(line, " UP  (was down "); str_cat(line, dur); str_cat(line, ")\n"); }
    f = Open((STRPTR)cfg.logfile, MODE_READWRITE);
    if (f) {
        Seek(f, 0, OFFSET_END);
        Write(f, line, (LONG)strlen(line));
        Close(f);
    }
}

/* record one result: ev 1 = went down, 2 = came up */
static void host_result(host_t *h, int ok, ULONG rtt_us)
{
    ULONG now = now_secs();
    int ev = 0;
    ULONG outage = 0;
    h->sent++;
    if (!ok) {
        h->fails++;
        if (h->fails == 1) h->fail_start = now;
        h->has_last = 0;
        if (h->fails >= (ULONG)cfg.down_after && h->state != 2) {
            h->state = 2;
            h->down_since = h->fail_start;
            ev = 1;
        }
    } else {
        h->recv++;
        h->last_us = rtt_us;
        h->has_last = 1;
        if (h->avg_total > 0x7fffffffUL) { h->avg_total >>= 1; h->avg_n >>= 1; }
        h->avg_total += rtt_us;
        h->avg_n++;
        h->recent[h->irecent] = rtt_us;
        h->irecent = (h->irecent + 1) % RECENT;
        if (h->nrecent < RECENT) h->nrecent++;
        if (!h->has_minmax || rtt_us < h->min_us) h->min_us = rtt_us;
        if (!h->has_minmax || rtt_us > h->max_us) h->max_us = rtt_us;
        h->has_minmax = 1;
        h->fails = 0;
        h->last_ok = now;
        if (h->state == 2) { outage = now - h->down_since; h->down_since = 0; ev = 2; }
        h->state = 1;
    }
    if (ev) {
        if (cfg.beep) con_write("\x07");
        log_event(h, ev, outage);
    }
}

/* --------------------------------------------------------- hosts file */

static void load_hosts(const char *path)
{
    static char buf[8192];
    BPTR f = Open((STRPTR)path, MODE_OLDFILE);
    LONG n;
    char *p, *end;
    if (!f) return;
    n = Read(f, buf, sizeof buf - 1);
    Close(f);
    if (n <= 0) return;
    buf[n] = 0;
    p = buf;
    end = buf + n;
    while (p < end) {
        char *line = p, *name, *label, *e;
        while (p < end && *p != '\n') p++;
        *p++ = 0;
        e = line;
        while (*e && *e != '#') e++;
        *e = 0;
        while (is_space(*line)) line++;
        if (!*line) continue;
        name = line;
        while (*line && !is_space(*line)) line++;
        if (*line) *line++ = 0;
        while (is_space(*line)) line++;
        label = line;
        e = label + strlen(label);
        while (e > label && is_space(e[-1])) *--e = 0;
        if (!host_exists(name)) host_add(name, label);
    }
}

static int save_hosts(const char *path)
{
    BPTR f = Open((STRPTR)path, MODE_NEWFILE);
    int i;
    const char *hdr = "# multiping hosts: one per line, optional label after the host\n";
    if (!f) return 0;
    Write(f, (APTR)hdr, (LONG)strlen(hdr));
    for (i = 0; i < g_nhosts; i++) {
        char line[128];
        str_copy(line, g_hosts[i].name, sizeof line);
        if (g_hosts[i].label[0]) { str_cat(line, " "); str_cat(line, g_hosts[i].label); }
        str_cat(line, "\n");
        Write(f, line, (LONG)strlen(line));
    }
    Close(f);
    return 1;
}

/* --------------------------------------------------------------- icmp */

static UWORD csum(const UBYTE *p, int len)
{
    ULONG sum = 0;
    while (len > 1) { sum += ((ULONG)p[0] << 8) | p[1]; p += 2; len -= 2; }
    if (len) sum += (ULONG)p[0] << 8;
    while (sum >> 16) sum = (sum & 0xffff) + (sum >> 16);
    return (UWORD)~sum;
}

static int resolve(host_t *h, ULONG now)
{
    struct hostent *he;
    ULONG a;
    if (h->addr) return 1;
    if (now < h->next_resolve) return 0;
    h->next_resolve = now + 30000;              /* retry DNS every 30 s */
    a = inet_addr(h->name);
    if (a != INADDR_NONE) { h->addr = a; return 1; }
    he = gethostbyname(h->name);                /* blocks while the resolver works */
    if (he && he->h_addr_list && he->h_addr_list[0] && he->h_length == 4) {
        memcpy(&h->addr, he->h_addr_list[0], 4);
        return 1;
    }
    return 0;
}

static int send_ping(host_t *h)
{
    UBYTE pkt[64];
    struct sockaddr_in sa;
    UWORD c;
    memset(pkt, 0, sizeof pkt);
    h->seq++;
    pkt[0] = 8;                                 /* echo request */
    pkt[4] = (UBYTE)(g_icmp_id >> 8);
    pkt[5] = (UBYTE)g_icmp_id;
    pkt[6] = (UBYTE)(h->seq >> 8);
    pkt[7] = (UBYTE)h->seq;
    pkt[8] = 'm'; pkt[9] = 'p';
    pkt[10] = (UBYTE)(h->uid >> 8);
    pkt[11] = (UBYTE)h->uid;
    c = csum(pkt, sizeof pkt);
    pkt[2] = (UBYTE)(c >> 8);
    pkt[3] = (UBYTE)c;
    memset(&sa, 0, sizeof sa);
    sa.sin_len = sizeof sa;
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = h->addr;
    GetSysTime(&h->t_sent);
    if (sendto(g_sock, pkt, (LONG)sizeof pkt, 0, (const struct sockaddr *)&sa, (LONG)sizeof sa) < 0)
        return 0;
    h->outstanding = 1;
    return 1;
}

static void handle_reply(void)
{
    static UBYTE buf[1600];
    struct sockaddr_in from;
    LONG fromlen = sizeof from, n;
    const UBYTE *p;
    int ihl;
    host_t *h;
    struct timeval now;
    UWORD seq, uid;

    n = recvfrom(g_sock, buf, (LONG)sizeof buf, 0, (struct sockaddr *)&from, &fromlen);
    if (n < 20) return;
    ihl = (buf[0] & 0x0f) * 4;                  /* raw sockets deliver the IP header too */
    if (n < ihl + 12) return;
    p = buf + ihl;
    if (p[0] != 0) return;                      /* not an echo reply */
    if (((UWORD)p[4] << 8 | p[5]) != g_icmp_id) return;
    if (p[8] != 'm' || p[9] != 'p') return;
    seq = (UWORD)((UWORD)p[6] << 8 | p[7]);
    uid = (UWORD)((UWORD)p[10] << 8 | p[11]);
    h = host_by_uid(uid);
    if (!h || !h->outstanding || h->seq != seq) return;   /* late or duplicate */
    GetSysTime(&now);
    {
        LONG us = tv_diff_us(&now, &h->t_sent);
        if (us < 0) us = 0;
        h->outstanding = 0;
        host_result(h, 1, (ULONG)us);
    }
}

/* ------------------------------------------------------------ console */

static char g_prompt[48], g_buf[96], g_msg[160];
static int g_prompting;
static ULONG g_msg_until;

static void set_msg(const char *s) { str_copy(g_msg, s, sizeof g_msg); g_msg_until = now_ms() + 5000; }

static void do_save(void)
{
    if (cfg.nosave) return;
    if (save_hosts(cfg.file)) { char m[160]; str_copy(m, "saved ", sizeof m); str_cat(m, cfg.file); set_msg(m); }
    else { char m[160]; str_copy(m, "could not write ", sizeof m); str_cat(m, cfg.file); set_msg(m); }
}

static void prompt_done(void)
{
    char *text = g_buf;
    while (*text == ' ') text++;
    if (g_prompt[0] == 'A' && *text) {
        char *label = text;
        while (*label && *label != ' ') label++;
        if (*label) { *label++ = 0; while (*label == ' ') label++; }
        if (host_exists(text)) { char m[160]; str_copy(m, text, 100); str_cat(m, " is already in the list"); set_msg(m); }
        else if (!host_add(text, label)) set_msg("host list is full");
        else do_save();
    } else if (g_prompt[0] == 'D' && *text) {
        int k = host_find(text);
        if (k < 0) { char m[160]; str_copy(m, "no such host: ", sizeof m); str_cat(m, text); set_msg(m); }
        else {
            memmove(&g_hosts[k], &g_hosts[k + 1], (ULONG)(g_nhosts - k - 1) * sizeof g_hosts[0]);
            g_nhosts--;
            do_save();
        }
    }
    g_prompting = 0;
    g_buf[0] = 0;
}

/* returns 1 to quit */
static int handle_key(int c)
{
    int i;
    if (g_prompting) {
        if (c == 13 || c == 10) prompt_done();
        else if (c == 27) { g_prompting = 0; g_buf[0] = 0; }
        else if (c == 8 || c == 127) { ULONG l = strlen(g_buf); if (l) g_buf[l - 1] = 0; }
        else if (c >= 32 && c < 127) { ULONG l = strlen(g_buf); if (l < sizeof g_buf - 1) { g_buf[l] = (char)c; g_buf[l + 1] = 0; } }
        return 0;
    }
    switch (c) {
    case 'q': case 'Q': case 3: return 1;
    case 'a': case 'A': str_copy(g_prompt, "Add host [label]: ", sizeof g_prompt); g_prompting = 1; g_buf[0] = 0; break;
    case 'd': case 'D': str_copy(g_prompt, "Delete host (number or name): ", sizeof g_prompt); g_prompting = 1; g_buf[0] = 0; break;
    case 'r': case 'R': for (i = 0; i < g_nhosts; i++) host_reset(&g_hosts[i]); set_msg("statistics reset"); break;
    }
    return 0;
}

/* Reads pending console input: keys, and the window-size report that
 * answers our CSI 0 q request (CSI 1;1;rows;cols r). Returns 1 to quit. */
static int poll_console(void)
{
    static int in_csi;
    static char csi[32];
    static int csilen;
    int quit = 0;
    while (WaitForChar(g_in, 0)) {
        UBYTE c;
        if (Read(g_in, &c, 1) != 1) break;
        if (in_csi) {
            if (c >= 0x40 && c <= 0x7e) {
                in_csi = 0;
                csi[csilen] = 0;
                if (c == 'r') {                   /* window bounds report */
                    ULONG v[4] = { 0, 0, 0, 0 };
                    int k = 0;
                    const char *p = csi;
                    while (*p && k < 4) {
                        if (*p >= '0' && *p <= '9') v[k] = v[k] * 10 + (ULONG)(*p - '0');
                        else if (*p == ';') k++;
                        p++;
                    }
                    if (v[2] >= 5 && v[3] >= 40) { g_rows = (int)v[2]; g_cols = (int)v[3]; }
                }
            } else if (csilen < (int)sizeof csi - 1) csi[csilen++] = (char)c;
            continue;
        }
        if (c == 0x9b) { in_csi = 1; csilen = 0; continue; }
        if (handle_key(c)) quit = 1;
    }
    return quit;
}

/* ------------------------------------------------------------- render */

static void render(void)
{
    int i, hw, maxrows, nup = 0, ndown = 0, show_jtr;
    char tm[16], disp[100], s[32];
    ULONG now = now_secs();

    g_olen = 0;
    hw = 8;
    for (i = 0; i < g_nhosts; i++) {
        int l;
        host_display(&g_hosts[i], disp);
        l = (int)strlen(disp);
        if (l > hw) hw = l;
        if (g_hosts[i].state == 1) nup++;
        else if (g_hosts[i].state == 2) ndown++;
    }
    show_jtr = g_cols >= 100;
    {
        int fixed = 61 + (show_jtr ? 6 : 0);
        if (hw > g_cols - fixed) hw = g_cols - fixed;
        if (hw < 8) hw = 8;
    }
    clock_str(NULL, tm);

    o_str(CSI "H");
    o_sgr("1"); o_str("multiping"); o_sgr("0");
    fmt_u(s, (ULONG)g_nhosts); o_str("  "); o_str(s); o_str(" hosts   ");
    fmt_u(s, (ULONG)nup); o_str(s); o_str(" up   ");
    if (ndown) o_sgr("1;7");
    fmt_u(s, (ULONG)ndown); o_str(s); o_str(" down");
    if (ndown) o_sgr("0");
    o_str("   every "); fmt_u(s, cfg.interval_ms / 1000); o_str(s);
    if (cfg.interval_ms % 1000) { o_str("."); fmt_u(s, (cfg.interval_ms % 1000) / 100); o_str(s); }
    o_str("s, timeout "); fmt_u(s, cfg.timeout_ms); o_str(s); o_str("ms, ms   ");
    o_str(tm);
    o_str(CSI "K\n");

    o_col("#", 3, 0); o_str(" "); o_col("HOST", hw, 1); o_str(" "); o_col("STATUS", 11, 1);
    o_str(" "); o_col("SENT", 5, 0); o_str(" "); o_col("RECV", 5, 0); o_str(" "); o_col("LOSS", 6, 0);
    o_str(" "); o_col("LAST", 5, 0); o_str(" "); o_col("AVG", 5, 0); o_str(" "); o_col("MIN", 5, 0);
    o_str(" "); o_col("MAX", 5, 0);
    if (show_jtr) { o_str(" "); o_col("JTR", 5, 0); }
    o_str(" "); o_col("LASTOK", 6, 0);
    o_str(CSI "K\n");

    maxrows = g_rows - 4;
    if (maxrows < 1) maxrows = 1;
    for (i = 0; i < g_nhosts && i < maxrows; i++) {
        host_t *h = &g_hosts[i];
        char status[24], lastok[16];
        host_display(h, disp);
        if ((int)strlen(disp) > hw) { disp[hw - 1] = '~'; disp[hw] = 0; }
        if (h->state == 2) { char d[16]; fmt_dur(d, now - h->down_since); str_copy(status, "DOWN ", sizeof status); str_cat(status, d); }
        else if (h->state == 1) str_copy(status, " UP", sizeof status);
        else str_copy(status, " ...", sizeof status);
        if (h->last_ok == 0) str_copy(lastok, "never", sizeof lastok);
        else if (now - h->last_ok < 2) str_copy(lastok, "now", sizeof lastok);
        else fmt_dur(lastok, now - h->last_ok);

        fmt_u(s, (ULONG)i + 1); o_col(s, 3, 0); o_str(" ");
        if (h->state == 2) o_sgr("1;7"); else if (h->state == 1) o_sgr("33");
        o_col(disp, hw, 1);
        o_sgr("0");
        o_str(" ");
        if (h->state == 2) o_sgr("1;7"); else if (h->state == 1) o_sgr("33;1");
        o_col(status, 11, 1);
        o_sgr("0");
        o_str(" "); fmt_u(s, h->sent); o_col(s, 5, 0);
        o_str(" "); fmt_u(s, h->recv); o_col(s, 5, 0);
        if (h->sent) {
            ULONG t = (h->sent - h->recv) * 1000 / h->sent;    /* tenths of a percent */
            fmt_u(s, t / 10); { ULONG l = strlen(s); s[l] = '.'; s[l + 1] = (char)('0' + t % 10); s[l + 2] = '%'; s[l + 3] = 0; }
        } else str_copy(s, "-", sizeof s);
        o_str(" "); o_col(s, 6, 0);
        o_str(" "); fmt_ms(s, h->has_last, h->last_us); o_col(s, 5, 0);
        o_str(" "); fmt_ms(s, h->avg_n > 0, h->avg_n ? h->avg_total / h->avg_n : 0); o_col(s, 5, 0);
        o_str(" "); fmt_ms(s, h->has_minmax, h->min_us); o_col(s, 5, 0);
        o_str(" "); fmt_ms(s, h->has_minmax, h->max_us); o_col(s, 5, 0);
        if (show_jtr) {
            ULONG jit = 0;
            int k, first = (h->irecent - h->nrecent + RECENT) % RECENT;
            for (k = 1; k < h->nrecent; k++) {
                ULONG a = h->recent[(first + k) % RECENT], b = h->recent[(first + k - 1) % RECENT];
                jit += a > b ? a - b : b - a;
            }
            if (h->nrecent >= 2) jit /= (ULONG)(h->nrecent - 1);
            o_str(" "); fmt_ms(s, h->nrecent >= 2, jit); o_col(s, 5, 0);
        }
        o_str(" "); o_col(lastok, 6, 0);
        o_str(CSI "K\n");
    }
    if (g_nhosts > maxrows) {
        fmt_u(s, (ULONG)(g_nhosts - maxrows));
        o_str("... "); o_str(s); o_str(" more hosts (enlarge the window)"); o_str(CSI "K\n");
    }
    o_str(CSI "K\n");
    if (g_prompting) { o_sgr("1"); o_str(g_prompt); o_sgr("0"); o_str(g_buf); }
    else { o_str("[a]dd  [d]elete  [r]eset stats  [q]uit   "); o_str(g_msg); }
    o_str(CSI "K" CSI "J");
    o_str(g_prompting ? CSI "1 p" : CSI "0 p");    /* cursor on / off */
    o_flush();
}

/* --------------------------------------------------------------- main */

static void usage(void)
{
    con_write("multiping " VERSION " - ping many hosts at once, one line per host\n"
              "usage: multiping HOSTS/M,FILE/K,INTERVAL=I/K/N,TIMEOUT=T/K/N,DOWNAFTER=D/K/N,LOG/K,BEEP/S,NOSAVE/S,NOCOLOR/S\n"
              "  HOSTS      hosts to ping, host or host=label\n"
              "  FILE       hosts file, one per line with optional label (default S:multiping_hosts)\n"
              "  INTERVAL   seconds between pings per host (default 1)\n"
              "  TIMEOUT    milliseconds to wait for a reply (default 1000)\n"
              "  DOWNAFTER  consecutive misses before a host shows DOWN (default 1)\n"
              "  LOG        append UP/DOWN changes with timestamps to this file\n"
              "  BEEP       beep when a host goes down or comes back\n"
              "  NOSAVE     never rewrite the hosts file\n"
              "  NOCOLOR    plain text\n"
              "keys while running: a=add host  d=delete host  r=reset stats  q=quit\n");
}

static int open_timer(void)
{
    g_tport = CreateMsgPort();
    if (!g_tport) return 0;
    g_treq = (struct timerequest *)CreateIORequest(g_tport, sizeof(struct timerequest));
    if (!g_treq) return 0;
    if (OpenDevice("timer.device", UNIT_MICROHZ, (struct IORequest *)g_treq, 0) != 0) return 0;
    TimerBase = g_treq->tr_node.io_Device;
    return 1;
}

static void close_timer(void)
{
    if (TimerBase) CloseDevice((struct IORequest *)g_treq);
    if (g_treq) DeleteIORequest((struct IORequest *)g_treq);
    if (g_tport) DeleteMsgPort(g_tport);
    TimerBase = NULL;
}

int main(void)
{
    static const char template_[] = "HOSTS/M,FILE/K,INTERVAL=I/K/N,TIMEOUT=T/K/N,DOWNAFTER=D/K/N,LOG/K,BEEP/S,NOSAVE/S,NOCOLOR/S,VERBOSE/S";
    LONG args[10] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
    struct RDArgs *rda = NULL;
    int rc = RETURN_OK, i;
    ULONG last_render = 0, last_sizeq = 0;

    SysBase = *(struct ExecBase **)4UL;
    DOSBase = (struct DosLibrary *)OpenLibrary("dos.library", 37);
    if (!DOSBase) return RETURN_FAIL;

    if (WBStartupMsg) {
        g_con = Open("CON:0/20/640/220/multiping/CLOSE", MODE_NEWFILE);
        if (!g_con) { CloseLibrary((struct Library *)DOSBase); return RETURN_FAIL; }
        g_in = g_out = g_con;
    } else {
        g_in = Input();
        g_out = Output();
        TRACE("dos open, have Input/Output, calling ReadArgs");
        rda = ReadArgs((STRPTR)template_, args, NULL);
        if (!rda) {
            PrintFault(IoErr(), "multiping");
            usage();
            CloseLibrary((struct Library *)DOSBase);
            return RETURN_ERROR;
        }
        if (args[1]) cfg.file = (const char *)args[1];
        if (args[2]) { LONG v = *(LONG *)args[2]; cfg.interval_ms = v < 1 ? 1000 : (ULONG)v * 1000; }
        if (args[3]) { LONG v = *(LONG *)args[3]; cfg.timeout_ms = v < 100 ? 100 : (ULONG)v; }
        if (args[4]) { cfg.down_after = *(LONG *)args[4]; if (cfg.down_after < 1) cfg.down_after = 1; }
        if (args[5]) cfg.logfile = (const char *)args[5];
        if (args[6]) cfg.beep = 1;
        if (args[7]) cfg.nosave = 1;
        if (args[8]) cfg.color = 0;
        if (args[9]) g_verbose = 1;
    }

    TRACE("ReadArgs done, loading hosts file");
    load_hosts(cfg.file);
    TRACE("hosts file done");
    if (args[0]) {
        char **hp = (char **)args[0];
        while (*hp) {
            char *name = *hp++, *eq = name;
            while (*eq && *eq != '=') eq++;
            if (*eq) *eq++ = 0; else eq = NULL;
            if (*name && !host_exists(name)) host_add(name, eq);
        }
    }
    if (g_nhosts == 0) {
        TRACE("no hosts: printing usage");
        usage();
        TRACE("usage printed");
        con_write("\nno hosts given and nothing in "); con_write(cfg.file); con_write("\n");
        rc = RETURN_ERROR;
        goto out;
    }

    TRACE("hosts loaded, opening timer.device");
    if (!open_timer()) { con_write("multiping: cannot open timer.device\n"); rc = RETURN_FAIL; goto out; }
    g_base_secs = now_secs();

    TRACE("opening bsdsocket.library");
    SocketBase = OpenLibrary("bsdsocket.library", 2);
    if (!SocketBase) {
        con_write("multiping: bsdsocket.library not found - is the TCP/IP stack running?\n");
        rc = RETURN_FAIL;
        goto out;
    }
    TRACE("opening raw ICMP socket");
    g_sock = socket(AF_INET, SOCK_RAW, IPPROTO_ICMP);
    if (g_sock < 0) {
        char s[16];
        fmt_u(s, (ULONG)Errno());
        con_write("multiping: cannot open a raw ICMP socket (errno "); con_write(s);
        con_write(") - the TCP/IP stack must allow raw sockets\n");
        rc = RETURN_FAIL;
        goto out;
    }
    g_icmp_id = (UWORD)((ULONG)FindTask(NULL) >> 2);
    TRACE("socket open, resolving + sending first pings (console goes RAW after this)");
    if (g_verbose) {
        for (i = 0; i < g_nhosts; i++) {
            char s[16];
            con_write("multiping: resolve "); con_write(g_hosts[i].name);
            if (resolve(&g_hosts[i], now_ms())) { con_write(" ok, send "); con_write(send_ping(&g_hosts[i]) ? "ok" : "FAILED"); }
            else con_write(" FAILED");
            fmt_u(s, (ULONG)Errno()); con_write(" (errno "); con_write(s); con_write(")\n");
        }
        {
            fd_set rf; struct timeval tv; ULONG sigs = SIGBREAKF_CTRL_C; LONG n; char s[16];
            FD_ZERO(&rf); FD_SET(g_sock, &rf); tv.tv_secs = 2; tv.tv_micro = 0;
            con_write("multiping: WaitSelect 2s...");
            n = WaitSelect(g_sock + 1, &rf, NULL, NULL, &tv, &sigs);
            fmt_u(s, (ULONG)n); con_write(" returned "); con_write(s); con_write("\n");
            if (n > 0) { con_write("multiping: recvfrom..."); handle_reply(); con_write(" done\n"); }
        }
        con_write("multiping: entering screen mode\n");
        Delay(100);
    }

    SetMode(g_in, 1);                                    /* RAW console: keys arrive immediately */
    if (cfg.color) con_write(CSI "0 p");                 /* cursor off */
    con_write("\x0c");                                   /* clear screen */
    con_write(CSI "0 q");                                /* ask the console for its size */

    for (i = 0; i < g_nhosts; i++) g_hosts[i].next_send = now_ms() + (ULONG)i * 20;   /* spread the first burst */

    for (;;) {
        ULONG now = now_ms();
        fd_set rf;
        struct timeval tv;
        ULONG sigs = SIGBREAKF_CTRL_C;
        LONG n;

        for (i = 0; i < g_nhosts; i++) {
            host_t *h = &g_hosts[i];
            if (h->outstanding) {
                struct timeval t;
                GetSysTime(&t);
                if ((ULONG)tv_diff_us(&t, &h->t_sent) >= cfg.timeout_ms * 1000) { h->outstanding = 0; host_result(h, 0, 0); }
            }
            if ((LONG)(now - h->next_send) >= 0) {
                if (h->outstanding) { h->outstanding = 0; host_result(h, 0, 0); }
                if (!resolve(h, now) || !send_ping(h)) host_result(h, 0, 0);
                h->next_send = now + cfg.interval_ms;
            }
        }

        FD_ZERO(&rf);
        FD_SET(g_sock, &rf);
        tv.tv_secs = 0;
        tv.tv_micro = 100000;
        n = WaitSelect(g_sock + 1, &rf, NULL, NULL, &tv, &sigs);
        if (n < 0 && (sigs & SIGBREAKF_CTRL_C)) break;
        if (n > 0 && FD_ISSET(g_sock, &rf)) handle_reply();
        if (SetSignal(0, SIGBREAKF_CTRL_C) & SIGBREAKF_CTRL_C) break;

        if (poll_console()) break;
        now = now_ms();
        if (g_msg[0] && (LONG)(now - g_msg_until) > 0) g_msg[0] = 0;
        if (now - last_sizeq >= 5000) { con_write(CSI "0 q"); last_sizeq = now; }
        if (now - last_render >= 250) { render(); last_render = now; }
    }

    con_write(CSI "0m" CSI " p" "\x0c");                 /* colours off, cursor on, clear */
    SetMode(g_in, 0);

out:
    TRACE("cleanup: socket");
    if (g_sock >= 0) CloseSocket(g_sock);
    if (SocketBase) CloseLibrary(SocketBase);
    TRACE("cleanup: timer");
    close_timer();
    TRACE("cleanup: FreeArgs");
    if (rda) FreeArgs(rda);
    if (g_con) Close(g_con);
    TRACE("cleanup: closing dos, returning");
    CloseLibrary((struct Library *)DOSBase);
    return rc;
}
