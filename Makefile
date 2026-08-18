CC       ?= cc
SWIFTC   ?= swiftc
CFLAGS   ?= -O2 -Wall -Wextra
SWIFTFLAGS ?= -O
LDFLAGS   = -framework CoreGraphics -framework CoreFoundation -framework IOKit

PREFIX   ?= /usr/local
CLI_BIN  := displayarrange
APP_NAME := DisplayArrange
APP_DIR  := $(APP_NAME).app

CORE     := core/DisplayCore.c core/DisplayCore.h

.PHONY: all cli app clean install uninstall

all: cli app

# ---------------------------------------------------------------- CLI
cli: $(CLI_BIN)

$(CLI_BIN): cli/cli.c $(CORE)
	$(CC) $(CFLAGS) -o $@ cli/cli.c core/DisplayCore.c $(LDFLAGS)

# ---------------------------------------------------------------- App
app: $(APP_DIR)

$(APP_DIR): app/DisplayArrangeApp.swift $(CORE) app/Info.plist
	@echo "Building $(APP_DIR)..."
	@mkdir -p $(APP_DIR)/Contents/MacOS
	@mkdir -p $(APP_DIR)/Contents/Resources
	$(SWIFTC) $(SWIFTFLAGS) \
	    -import-objc-header core/DisplayCore.h \
	    -o $(APP_DIR)/Contents/MacOS/$(APP_NAME) \
	    app/DisplayArrangeApp.swift core/DisplayCore.c \
	    -framework AppKit -framework ServiceManagement \
	    -framework UserNotifications \
	    -framework CoreGraphics -framework CoreFoundation -framework IOKit
	@cp app/Info.plist $(APP_DIR)/Contents/
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
