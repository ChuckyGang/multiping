# multiping - see multiping.c for details
CC      ?= cc
CFLAGS  ?= -O2 -Wall -Wextra
MINGW   ?= x86_64-w64-mingw32-gcc

all: multiping

multiping: multiping.c
	$(CC) $(CFLAGS) -o $@ multiping.c -lpthread -lm

# Windows cross-build (apt install mingw-w64). Static: one .exe, nothing else needed.
multiping.exe: multiping.c
	$(MINGW) $(CFLAGS) -static -o $@ multiping.c -liphlpapi -lws2_32

windows: multiping.exe

# native macOS build: cc -O2 -o multiping multiping.c  (same as "all")

clean:
	rm -f multiping multiping.exe

.PHONY: all windows clean

amiga:
	$(MAKE) -C amiga

.PHONY: amiga
