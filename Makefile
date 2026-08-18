CC       ?= cc
SWIFTC   ?= swiftc
CFLAGS   ?= -O2 -Wall -Wextra
SWIFTFLAGS ?= -O
LDFLAGS   = -framework CoreGraphics -framework CoreFoundation -framework IOKit

PREFIX   ?= /usr/local
CLI_BIN  := displayarrange
APP_NAME := DisplayArrange
APP_DIR  := $(APP_NAME).app

.PHONY: all cli app clean install uninstall

all: cli app

# ---------------------------------------------------------------- CLI
cli: $(CLI_BIN)

$(CLI_BIN): cli.c DisplayCore.c DisplayCore.h
	$(CC) $(CFLAGS) -o $@ cli.c DisplayCore.c $(LDFLAGS)

# ---------------------------------------------------------------- App
app: $(APP_DIR)

$(APP_DIR): DisplayArrangeApp.swift DisplayCore.c DisplayCore.h Info.plist
	@echo "Building $(APP_DIR)..."
	@mkdir -p $(APP_DIR)/Contents/MacOS
	@mkdir -p $(APP_DIR)/Contents/Resources
	$(SWIFTC) $(SWIFTFLAGS) \
	    -import-objc-header DisplayCore.h \
	    -o $(APP_DIR)/Contents/MacOS/$(APP_NAME) \
	    DisplayArrangeApp.swift DisplayCore.c \
	    -framework AppKit -framework ServiceManagement \
	    -framework UserNotifications \
	    -framework CoreGraphics -framework CoreFoundation -framework IOKit
	@cp Info.plist $(APP_DIR)/Contents/
	@echo "Built $(APP_DIR)"

# ---------------------------------------------------------------- Housekeeping
clean:
	rm -f $(CLI_BIN)
	rm -rf $(CLI_BIN).dSYM
	rm -rf $(APP_DIR)

install: cli
	mkdir -p $(DESTDIR)$(PREFIX)/bin
	cp $(CLI_BIN) $(DESTDIR)$(PREFIX)/bin/

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/$(CLI_BIN)
