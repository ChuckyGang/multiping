# multiping for AmigaOS - Bartman/Abyss ELF toolchain (m68k-amiga-elf-gcc,
# -nostdlib, elf2hunk). Same recipe as BartTemp/AmiPart.
#
#   make                                   (uses the VS Code extension path below)
#   make BARTMAN=/path/to/amiga-debug/bin/linux

BARTMAN ?= /home/john/.vscode/extensions/bartmanabyss.amiga-debug-1.8.2/bin/linux

program = out/multiping

CC       := $(BARTMAN)/opt/bin/m68k-amiga-elf-gcc
AS       := $(BARTMAN)/opt/bin/m68k-amiga-elf-as
ELF2HUNK := $(BARTMAN)/elf2hunk
SDKDIR   := $(BARTMAN)/opt/m68k-amiga-elf/sys-include

CCFLAGS = -MP -MMD -m68000 -O2 -nostdlib \
          -Wall -Wextra -Wno-unused-function -Wno-pointer-sign -Wno-array-bounds -Wno-volatile-register-var \
          -DNO_INLINE_STDARG \
          -fomit-frame-pointer -fno-tree-loop-distribution \
          -fno-exceptions -ffunction-sections -fdata-sections \
          -Isrc -I$(SDKDIR)

ASFLAGS = -mcpu=68000 --register-prefix-optional -I$(SDKDIR)
LDFLAGS = -Wl,--emit-relocs,--gc-sections,-Ttext=0,-Map=$(program).map

$(shell mkdir -p obj out)

objects := obj/main.o obj/gcc8_c_support.o obj/gcc8_a_support.o

.PHONY: all clean

all: $(program)

$(program): $(program).elf
	$(info Elf2Hunk $@)
	@$(ELF2HUNK) $(program).elf $(program)

$(program).elf: $(objects)
	$(info Linking $@)
	@$(CC) $(CCFLAGS) $(LDFLAGS) $(objects) -o $@

obj/main.o: src/main.c src/bsdsocket_lp.h
	$(info Compiling $<)
	@$(CC) $(CCFLAGS) -c -o $@ $<

obj/gcc8_c_support.o: support/gcc8_c_support.c
	$(info Compiling $<)
	@$(CC) $(CCFLAGS) -c -o $@ $<

obj/gcc8_a_support.o: support/gcc8_a_support.s
	$(info Assembling $<)
	@$(AS) $(ASFLAGS) --MD obj/gcc8_a_support.d -o $@ $<

clean:
	rm -f obj/* out/*

-include obj/main.d obj/gcc8_c_support.d obj/gcc8_a_support.d
