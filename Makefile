CC = gcc

UNAME_S := $(shell uname -s)

GAME    ?= stub
BINDIR  = bin
CROSS   ?=       # set CROSS=1 for Windows cross-compile from Linux

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
              -Iinclude \
              -include generated/$(GAME)_embedded_data.h \
              -I/usr/i686-w64-mingw32/sys-root/mingw/include \
              -I/usr/i686-w64-mingw32/sys-root/mingw/include/SDL2 \
              -DSDL_MAIN_HANDLED

else ifeq ($(UNAME_S),Linux)

    PLATFORM = linux
    TARGET  = $(BINDIR)/$(GAME)
    PYTHON  = python3
    LDFLAGS = $(shell sdl2-config --libs) -lm
    CFLAGS  = -O2 -Wall -Wextra \
              -Wno-unused-parameter \
              -Wno-unused-variable \
              -Iinclude \
              -include generated/$(GAME)_embedded_data.h \
              $(shell sdl2-config --cflags)

else

    PLATFORM = windows
    TARGET  = $(BINDIR)/$(GAME).exe
    PYTHON  = python
    LDFLAGS = $(shell sdl2-config --libs 2>NUL || echo -L/mingw64/lib -lSDL2main -lSDL2) \
               -lmingw32 -lm
    CFLAGS  = -O2 -Wall -Wextra \
              -Wno-unused-parameter \
              -Wno-unused-variable \
              -Iinclude \
              -include generated/$(GAME)_embedded_data.h \
              $(shell sdl2-config --cflags)

endif

OBJDIR  = build/$(PLATFORM)/$(GAME)

EMBED_SRC = generated/$(GAME)_embedded_data.c
EMBED_HDR = generated/$(GAME)_embedded_data.h

RUNNER_SRCS = \
    memory.c \
    cpu_interp.c \
    ppu.c \
    apu.c \
    mapper.c \
    runner.c

FULL_SRC     = generated/$(GAME)_full.c
DISPATCH_SRC = generated/$(GAME)_dispatch.c

ifeq ($(wildcard $(DISPATCH_SRC)),)
    GAME_SRCS = $(FULL_SRC)
else
    GAME_SRCS = $(FULL_SRC) $(DISPATCH_SRC)
endif

ifeq ($(wildcard $(EMBED_SRC)),)
    ALL_SRCS = $(RUNNER_SRCS) $(GAME_SRCS)
else
    ALL_SRCS = $(RUNNER_SRCS) $(GAME_SRCS) $(EMBED_SRC)
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

.PHONY: all compile clean recomp dirs

all: recomp

compile: dirs $(TARGET)

$(TARGET): $(OBJS)
	@echo [LINK] $@
	$(CC) $(OBJS) -o $@ $(LDFLAGS)
	@echo Build successful: $@

$(OBJDIR)/%.o: %.c
	@$(MKDIR_OBJ)
	@echo [CC] $<
	$(CC) $(CFLAGS) -c $< -o $@

dirs:
	@$(MKDIR_DIRS)

# Generate embedded data from ROM
gen_embed:
	$(PYTHON) tools/extract_rom_data.py $(ROM) --game $(GAME) --out generated


# Recompile ROM, then build
# Usage: make GAME=NesGame  (or: make recomp GAME=NesGame)
#   ROM     — optional, defaults to rom/$(GAME).nes
#   ASM     — optional, defaults to asm/$(GAME).asm if that file exists
#   CFG     — always cfg/$(GAME).cfg
ROM     ?= rom/$(GAME).nes
ASM_FLAG = $(if $(ASM),--asm asm/$(ASM),$(if $(wildcard asm/$(GAME).asm),--asm asm/$(GAME).asm))

recomp:
ifndef GAME
	$(error GAME not set. Usage: make GAME=NesGame)
endif
	$(MAKE) gen_embed ROM=$(ROM) GAME=$(GAME)
	$(PYTHON) tools/nesrecomp.py $(ROM) --out generated --game $(GAME) --cfg cfg/$(GAME).cfg $(ASM_FLAG)
	$(MAKE) compile GAME=$(GAME)

# Clean
clean:
	@$(CLEAN_CMD)
	@echo Cleaned.
