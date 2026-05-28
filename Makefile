CC = gcc

UNAME_S := $(shell uname -s)

GAME    ?= stub
BINDIR  = bin
OBJDIR  = build
CROSS   ?=       # set CROSS=1 for Windows cross-compile from Linux

# ============================================================
#  Platform-specific
# ============================================================
ifeq ($(CROSS),1)

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
                mkdir -p $(BINDIR) $(OBJDIR) generated,\
                (if not exist $(BINDIR) mkdir $(BINDIR)) & \
                (if not exist $(OBJDIR) mkdir $(OBJDIR)) & \
                (if not exist generated mkdir generated))

# Clean
CLEAN_CMD = $(if $(filter Linux,$(UNAME_S)),\
                rm -rf $(OBJDIR) $(BINDIR),\
                (if exist $(OBJDIR) rmdir /S /Q $(OBJDIR)) & \
                (if exist $(BINDIR) rmdir /S /Q $(BINDIR)))

# ============================================================
#  Targets
# ============================================================

.PHONY: all clean recomp dirs

all: dirs $(TARGET)

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
	$(PYTHON) tools/extract_nes_data.py $(ROM) --game $(GAME) --out generated

# Recompile ROM, then build
recomp:
ifndef ROM
	$(error ROM not set)
endif
ifndef GAME
	$(error GAME not set)
endif
	$(MAKE) gen_embed ROM=$(ROM) GAME=$(GAME)
	$(PYTHON) nesrecomp.py $(ROM) --out generated --game $(GAME) --cfg $(GAME).cfg $(if $(ASM),--asm $(ASM))
	$(MAKE) GAME=$(GAME)

# Clean
clean:
	@$(CLEAN_CMD)
	@echo Cleaned.
