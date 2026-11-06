/*
 * Minimal bsdsocket.library bindings for the Bartman -nostdlib toolchain.
 *
 * The SDK's inline/bsdsocket.h uses MIT assembler syntax that this
 * toolchain's assembler rejects, and its network headers expect a C
 * library. The few calls multiping needs are declared here with the
 * NDK's LP macros instead, together with the 4.4BSD structures that
 * AmiTCP, Miami, Genesis and Roadshow all use.
 */
#ifndef BSDSOCKET_LP_H
#define BSDSOCKET_LP_H

#include <exec/types.h>
#include <devices/timer.h>
#include <inline/macros.h>

extern struct Library *SocketBase;

#define AF_INET       2
#define SOCK_RAW      3
#define IPPROTO_ICMP  1
#define INADDR_NONE   0xffffffffUL

struct in_addr { ULONG s_addr; };

struct sockaddr_in {
    UBYTE sin_len;
    UBYTE sin_family;
    UWORD sin_port;
    struct in_addr sin_addr;
    UBYTE sin_zero[8];
};

struct sockaddr {
    UBYTE sa_len;
    UBYTE sa_family;
    UBYTE sa_data[14];
};

struct hostent {
    char  *h_name;
    char **h_aliases;
    LONG   h_addrtype;
    LONG   h_length;
    char **h_addr_list;
};

typedef struct { ULONG fds_bits[2]; } fd_set;          /* FD_SETSIZE 64 */
#define FD_ZERO(s)      ((s)->fds_bits[0] = (s)->fds_bits[1] = 0)
#define FD_SET(n, s)    ((s)->fds_bits[(n) >> 5] |= 1UL << ((n) & 31))
#define FD_ISSET(n, s)  (((s)->fds_bits[(n) >> 5] & (1UL << ((n) & 31))) != 0)

#define socket(dom, type, proto) \
    LP3(30, LONG, socket, LONG, (dom), d0, LONG, (type), d1, LONG, (proto), d2, , SocketBase)
#define CloseSocket(s) \
    LP1(120, LONG, CloseSocket, LONG, (s), d0, , SocketBase)
#define sendto(s, buf, len, flags, to, tolen) \
    LP6(60, LONG, sendto, LONG, (s), d0, APTR, (buf), a0, LONG, (len), d1, LONG, (flags), d2, \
        const struct sockaddr *, (to), a1, LONG, (tolen), d3, , SocketBase)
#define recvfrom(s, buf, len, flags, from, fromlen) \
    LP6(72, LONG, recvfrom, LONG, (s), d0, APTR, (buf), a0, LONG, (len), d1, LONG, (flags), d2, \
        struct sockaddr *, (from), a1, LONG *, (fromlen), a2, , SocketBase)
#define WaitSelect(nfds, r, w, e, tv, sigs) \
    LP6(126, LONG, WaitSelect, LONG, (nfds), d0, fd_set *, (r), a0, fd_set *, (w), a1, fd_set *, (e), a2, \
        struct timeval *, (tv), a3, ULONG *, (sigs), d1, , SocketBase)
#define gethostbyname(name) \
    LP1(210, struct hostent *, gethostbyname, const char *, (name), a0, , SocketBase)
#define Errno() \
    LP0(162, LONG, Errno, , SocketBase)
#define inet_addr(cp) \
    LP1(180, ULONG, inet_addr, const char *, (cp), a0, , SocketBase)

#endif
