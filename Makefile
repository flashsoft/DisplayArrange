CC      ?= cc
CFLAGS  ?= -O2 -Wall -Wextra
LDFLAGS  = -framework CoreGraphics -framework CoreFoundation -framework IOKit

PREFIX  ?= /usr/local
BIN     := displayarrange

.PHONY: all clean install uninstall

all: $(BIN)

$(BIN): displayarrange.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

clean:
	rm -f $(BIN)
	rm -rf $(BIN).dSYM

install: $(BIN)
	mkdir -p $(DESTDIR)$(PREFIX)/bin
	cp $(BIN) $(DESTDIR)$(PREFIX)/bin/

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/$(BIN)
