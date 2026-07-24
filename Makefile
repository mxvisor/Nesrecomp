CC = gcc

UNAME_S := $(shell uname -s)

GAME          ?=
BINDIR         = bin
CROSS         ?=       # set CROSS=1 for Windows cross-compile from Linux
DEFAULT_SCALE ?= 1

# ============================================================
#  Platform-specific
# ============================================================
ifeq ($(CROSS),1)

    PLATFORM = win32
    TARGET  = $(BINDIR)/$(GAME).exe
    PYTHON  = python3
    CC      = i686-w64-mingw32-gcc
    LDFLAGS = -static-libgcc -lSDL2main -lSDL2 -lmingw32 -lm -lwinpthread -mwindows
    CFLAGS  = -O2 -Wall -Wextra \
              -Wno-unused-parameter \
              -Wno-unused-variable \
              -Isrc/include \
              -include generated/$(GAME)_embedded_data.h \
              -I/usr/i686-w64-mingw32/sys-root/mingw/include \
              -I/usr/i686-w64-mingw32/sys-root/mingw/include/SDL2 \
              -DSDL_MAIN_HANDLED \
              -DGAME_NAME=\"$(GAME)\" \
              -DDEFAULT_SCALE=$(DEFAULT_SCALE)

else ifeq ($(UNAME_S),Linux)

    PLATFORM = linux
    TARGET  = $(BINDIR)/$(GAME)
    PYTHON  = python3
    LDFLAGS = $(shell sdl2-config --libs) -lm
    CFLAGS  = -O2 -Wall -Wextra \
              -Wno-unused-parameter \
              -Wno-unused-variable \
              -Isrc/include \
              -include generated/$(GAME)_embedded_data.h \
              $(shell sdl2-config --cflags) \
              -DGAME_NAME=\"$(GAME)\" \
              -DDEFAULT_SCALE=$(DEFAULT_SCALE)

else

    PLATFORM = windows
    TARGET  = $(BINDIR)/$(GAME).exe
    PYTHON  = python
    LDFLAGS = $(shell sdl2-config --libs 2>NUL || echo -L/mingw64/lib -lSDL2main -lSDL2) \
               -lmingw32 -lm
    CFLAGS  = -O2 -Wall -Wextra \
              -Wno-unused-parameter \
              -Wno-unused-variable \
              -Isrc/include \
              -include generated/$(GAME)_embedded_data.h \
              $(shell sdl2-config --cflags) \
              -DGAME_NAME=\"$(GAME)\" \
              -DDEFAULT_SCALE=$(DEFAULT_SCALE)

endif

OBJDIR  = build/$(PLATFORM)/$(GAME)

# ============================================================
#  GPL FCEUX vendor oracle (--interp=fceux_vendor)
#  The vendored FCEUX core (GPL) lives in a gitignored nogpl/ dir so the tree
#  builds & commits GPL-free. Auto-detected: present → compiled into every build
#  (see ALL_SRCS) and -DHAVE_VENDOR enables the vendor CPU path + CLI flag in
#  runner.c. Absent → VENDOR_SRCS empty, GPL-free build, --interp=fceux_vendor
#  disabled. NB: -DHAVE_VENDOR must track VENDOR_SRCS linkage exactly — defining
#  it without linking the vendor .o gives undefined refs (runner.c #ifdefs it in).
# ============================================================
VENDOR_SRCS := $(wildcard nogpl/x6502_vendor.c nogpl/ppu_vendor.c nogpl/apu_vendor.c)
ifneq ($(VENDOR_SRCS),)
    CFLAGS += -DHAVE_VENDOR -Inogpl
endif

EMBED_SRC = generated/$(GAME)_embedded_data.c
EMBED_HDR = generated/$(GAME)_embedded_data.h

RUNNER_SRCS = \
    src/memory.c \
    src/cpu_interp.c \
    src/ppu.c \
    src/apu.c \
    src/apu_fceux.c \
    src/mapper.c \
    src/fm2_player.c \
    src/runner.c

FULL_SRC     = generated/$(GAME)_full.c
DISPATCH_SRC = generated/$(GAME)_dispatch.c

# INTERP=1: interpreter-only build for demo-sync testing — link the tiny
# src/stub_full.c (call_by_address -> cpu_interp_run) instead of the generated
# recompiled code, and skip the discover step. The recompiled code is never
# executed under --interp anyway, and for bank-aware games (e.g. Mermaid:
# ~24500 functions) compiling the giant generated/_full.c with -O2 exhausts RAM.
ifeq ($(INTERP),1)
    GAME_SRCS = src/stub_full.c
else ifeq ($(wildcard $(DISPATCH_SRC)),)
    GAME_SRCS = $(FULL_SRC)
else
    GAME_SRCS = $(FULL_SRC) $(DISPATCH_SRC)
endif

# $(VENDOR_SRCS) is compiled into EVERY build (empty when nogpl/ is absent) so
# the vendor .o linkage always matches the -DHAVE_VENDOR flag above.
ifeq ($(wildcard $(EMBED_SRC)),)
    ALL_SRCS = $(RUNNER_SRCS) $(GAME_SRCS) $(VENDOR_SRCS)
else
    ALL_SRCS = $(RUNNER_SRCS) $(GAME_SRCS) $(VENDOR_SRCS) $(EMBED_SRC)
endif

OBJS = $(patsubst %.c,$(OBJDIR)/%.o,$(ALL_SRCS))

# ============================================================
#  Platform-specific shell commands
#  $(if ...) is expanded by MAKE, shell only sees the result
# ============================================================
# Create directory for a single object file (dir is $(dir $@))
MKDIR_OBJ = $(if $(filter Linux,$(UNAME_S)),\
                mkdir -p $(dir $@),\
                if not exist "$(subst /,\,$(dir $@))" mkdir "$(subst /,\,$(dir $@))")

# Create top-level build dirs
MKDIR_DIRS = $(if $(filter Linux,$(UNAME_S)),\
                mkdir -p $(BINDIR) $(OBJDIR) generated cfg,\
                (if not exist $(BINDIR) mkdir $(BINDIR)) & \
                (if not exist $(OBJDIR) mkdir $(OBJDIR)) & \
                (if not exist generated mkdir generated) & \
                (if not exist cfg mkdir cfg))

# Clean
CLEAN_CMD = $(if $(filter Linux,$(UNAME_S)),\
                rm -rf $(OBJDIR) $(BINDIR),\
                (if exist $(OBJDIR) rmdir /S /Q $(OBJDIR)) & \
                (if exist $(BINDIR) rmdir /S /Q $(BINDIR)))

# ============================================================
#  Targets
# ============================================================

.PHONY: all help roms compile clean recomp dirs gen_embed parse_asm discover

all:
ifeq ($(GAME),)
	@$(MAKE) help
else
	@$(MAKE) recomp GAME=$(GAME)
endif

help:
	@echo "Usage:"
	@echo "  make GAME=MyGame            — full pipeline (embed+recomp+compile)"
	@echo "  make roms                   — build all ROMs found in rom/*.nes"
	@echo "  make compile GAME=MyGame    — compile only (skip recomp)"
	@echo "  make discover GAME=MyGame   — run static recompiler only"
	@echo "  make clean   GAME=MyGame    — remove build artifacts"
	@echo ""
	@echo "Options:"
	@echo "  ROM=path/to/game.nes        — override ROM path (default: rom/GAME.nes)"
	@echo "  ASM=MyGame.asm              — ca65 source for extra BFS seeds"
	@echo "  ORPHAN=N                    — orphan-window size (default 3)"
	@echo "  CROSS=1                     — cross-compile for Windows (MinGW)"
	@echo "  DEFAULT_SCALE=N             — compile-time window scale (default 1)"

# Build every *.nes found in rom/ — uses filename stem as GAME name
roms:
	@found=0; \
	for nes in rom/*.nes; do \
	    [ -f "$$nes" ] || continue; \
	    found=1; \
	    game=$$(basename "$$nes" .nes); \
	    echo ""; \
	    echo "========================================"; \
	    echo " Building: $$game"; \
	    echo "========================================"; \
	    $(MAKE) GAME=$$game ROM=$$nes || exit 1; \
	done; \
	[ $$found -eq 1 ] || echo "No ROMs found in rom/"

compile: dirs $(TARGET)

$(TARGET): $(OBJS)
	@echo [LINK] $@
	$(CC) $(OBJS) -o $@ $(LDFLAGS)
	@echo Build successful: $@

$(OBJDIR)/%.o: %.c
	@$(MKDIR_OBJ)
	@echo [CC] $<
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJDIR)/src/%.o: src/%.c
	@$(MKDIR_OBJ)
	@echo [CC] $<
	$(CC) $(CFLAGS) -c $< -o $@

dirs:
	@$(MKDIR_DIRS)

# Generate embedded data from ROM
gen_embed:
	$(PYTHON) tools/extract_rom_data.py $(ROM) --game $(GAME) --out generated


# ============================================================
#  Recompile pipeline
#  Usage: make GAME=NesGame  (or: make recomp GAME=NesGame)
#    ROM    — optional, defaults to rom/$(GAME).nes
#    ASM    — optional ASM file name (without path); auto-detected from asm/$(GAME).asm
#    ORPHAN — orphan-window size (default 3)
# ============================================================
ROM          ?= rom/$(GAME).nes
ASM_FILE      = $(if $(ASM),asm/$(ASM),$(wildcard asm/$(GAME).asm))
ORPHAN_FLAG   = $(if $(ORPHAN),--orphan-window $(ORPHAN))

# Step 1: parse ca65 ASM labels → merge into cfg/$(GAME).cfg
parse_asm:
ifndef GAME
	$(error GAME not set)
endif
	$(if $(ASM_FILE),\
	  $(PYTHON) tools/asm_parser.py $(ASM_FILE) --cfg cfg/$(GAME).cfg,\
	  @echo "[parse_asm] no ASM file for $(GAME) — skipping")

# Step 2: run static recompiler (BFS discovery + C emit)
discover:
ifndef GAME
	$(error GAME not set)
endif
	$(PYTHON) tools/nesrecomp.py $(ROM) --out generated --game $(GAME) \
	    --cfg cfg/$(GAME).cfg $(ORPHAN_FLAG)

# Full pipeline: embed → parse_asm → discover → compile
recomp:
ifndef GAME
	$(error GAME not set. Usage: make GAME=NesGame)
endif
	$(MAKE) gen_embed ROM=$(ROM) GAME=$(GAME)
ifneq ($(INTERP),1)
	$(MAKE) parse_asm GAME=$(GAME) ASM=$(ASM) ROM=$(ROM)
	$(MAKE) discover  GAME=$(GAME) ASM=$(ASM) ROM=$(ROM) ORPHAN=$(ORPHAN)
endif
	$(MAKE) compile   GAME=$(GAME) INTERP=$(INTERP)

# Clean
clean:
	@$(CLEAN_CMD)
	@echo Cleaned.
