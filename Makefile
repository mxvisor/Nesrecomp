CC = gcc

UNAME_S := $(shell uname -s)

GAME    ?= stub
BINDIR  = bin
OBJDIR  = build

ifeq ($(UNAME_S),Linux)

    TARGET = $(BINDIR)/$(GAME)

    LDFLAGS = $(shell sdl2-config --libs) -lm

else

    TARGET = $(BINDIR)/$(GAME).exe

    LDFLAGS = $(shell sdl2-config --libs 2>NUL || echo -L/mingw64/lib -lSDL2main -lSDL2) \
               -lmingw32 -lm

endif

CFLAGS = -O2 -Wall -Wextra \
         -Wno-unused-parameter \
         -Wno-unused-variable \
         -Iinclude \
         -include generated/$(GAME)_embedded_data.h \
         $(shell sdl2-config --cflags)

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

.PHONY: all clean recomp dirs

all: dirs $(TARGET)

$(TARGET): $(OBJS)
	@echo [LINK] $@
	$(CC) $(OBJS) -o $@ $(LDFLAGS)
	@echo Build successful: $@

$(OBJDIR)/%.o: %.c
	@mkdir -p $(dir $@)
	@echo [CC] $<
	$(CC) $(CFLAGS) -c $< -o $@

dirs:
	@mkdir -p $(BINDIR)
	@mkdir -p $(OBJDIR)
	@mkdir -p generated

# Generate embedded data – also called by 'recomp'
gen_embed:
	python3 tools/extract_nes_data.py $(ROM) --game $(GAME) --out generated

recomp:
ifndef ROM
	$(error ROM not set)
endif
ifndef GAME
	$(error GAME not set)
endif
	$(MAKE) gen_embed ROM=$(ROM) GAME=$(GAME)
	python3 nesrecomp.py $(ROM) --out generated --game $(GAME)
	$(MAKE) GAME=$(GAME)

clean:
	rm -rf $(OBJDIR)
	rm -rf $(BINDIR)
	@echo Cleaned.
