CC = gcc

UNAME_S := $(shell uname -s)

GAME    ?= stub
BINDIR  = bin
OBJDIR  = build

ifeq ($(UNAME_S),Linux)

    TARGET = $(BINDIR)/nesrecomp

    LDFLAGS = $(shell sdl2-config --libs) -lm

else

    TARGET = $(BINDIR)/nesrecomp.exe

    LDFLAGS = $(shell sdl2-config --libs 2>NUL || echo -L/mingw64/lib -lSDL2main -lSDL2) \
               -lmingw32 -lm

endif

CFLAGS = -O2 -Wall -Wextra \
         -Wno-unused-parameter \
         -Wno-unused-variable \
         -Iinclude \
         $(shell sdl2-config --cflags)

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

ALL_SRCS = $(RUNNER_SRCS) $(GAME_SRCS)

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

recomp:
ifndef ROM
	$(error ROM not set)
endif
ifndef GAME
	$(error GAME not set)
endif
	python3 nesrecomp.py $(ROM) --out generated --game $(GAME)
	$(MAKE) GAME=$(GAME)

clean:
	rm -rf $(OBJDIR)
	rm -rf $(BINDIR)
	@echo Cleaned.