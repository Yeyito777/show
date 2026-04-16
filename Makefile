PREFIX   ?= $(HOME)/.local
BINDIR   ?= $(PREFIX)/bin
ZSHDIR   ?= $(PREFIX)/share/zsh/site-functions

CC       ?= gcc
CFLAGS   ?= -O2 -Wall -Wextra
PKG_DEPS  = sdl2 SDL2_image x11 cairo poppler-glib

PKG_CFLAGS := $(shell pkg-config --cflags $(PKG_DEPS))
PKG_LIBS   := $(shell pkg-config --libs   $(PKG_DEPS))

BIN = show
ZSH_COMPLETION = _show

all: $(BIN)

$(BIN): show.c
	$(CC) $(CFLAGS) $(PKG_CFLAGS) -o $@ $< $(PKG_LIBS)

install: $(BIN)
	install -Dm755 $(BIN) $(BINDIR)/$(BIN)
	install -Dm644 $(ZSH_COMPLETION) $(ZSHDIR)/$(ZSH_COMPLETION)

uninstall:
	rm -f $(BINDIR)/$(BIN)
	rm -f $(ZSHDIR)/$(ZSH_COMPLETION)

clean:
	rm -f $(BIN)

.PHONY: all install uninstall clean
