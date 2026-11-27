CC       ?= cc
SWIFTC   ?= swiftc
# A Command Line Tools SDK newer than the Xcode toolchain (e.g. CLT 27
# .tbd files naming arm64e.x1) breaks linking, so prefer the SDK bundled
# with Xcode when one is installed; fall back to the xcrun default.
ifeq ($(SDKROOT),)
XCODE_SDK := $(firstword $(wildcard /Applications/Xcode.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk))
SDKROOT   := $(if $(XCODE_SDK),$(XCODE_SDK),$(shell xcrun --show-sdk-path 2>/dev/null))
endif
SDKFLAGS  := -isysroot $(SDKROOT)
# Universal binary: Apple Silicon + Intel
ARCHES    = -arch arm64 -arch x86_64
CFLAGS   ?= -O2 -Wall -Wextra $(ARCHES)
SWIFTFLAGS ?= -O
LDFLAGS   = -framework CoreGraphics -framework CoreFoundation -framework IOKit

PREFIX   ?= /usr/local
CLI_BIN  := displayarrange
APP_NAME := DisplayArrange
DIST_DIR := dist
VERSION := $(shell cat VERSION)
# Inject version into Info.plist at build time
APP_DIR  := $(DIST_DIR)/$(APP_NAME).app

CORE     := core/DisplayCore.c core/DisplayCore.h

.PHONY: all cli app clean install uninstall test

all: cli app

# ---------------------------------------------------------------- CLI
cli: $(DIST_DIR)/$(CLI_BIN)

$(DIST_DIR)/$(CLI_BIN): cli/cli.c $(CORE)
	@mkdir -p $(DIST_DIR)
	$(CC) $(CFLAGS) $(SDKFLAGS) -o $@ cli/cli.c core/DisplayCore.c $(LDFLAGS)

# ---------------------------------------------------------------- App
app: $(APP_DIR)

$(APP_DIR): app/DisplayArrangeApp.swift $(CORE) app/Info.plist assets/AppIcon.icns scripts/install-update.sh VERSION
	@echo "Building $(APP_DIR) v$(VERSION)..."
	@mkdir -p $(APP_DIR)/Contents/MacOS
	@mkdir -p $(APP_DIR)/Contents/Resources
	@for arch in arm64 x86_64; do \
	    $(SWIFTC) $(SWIFTFLAGS) -target $$arch-apple-macosx13.0 \
	        -sdk $(SDKROOT) \
	        -import-objc-header core/DisplayCore.h \
	        -o $(APP_DIR)/Contents/MacOS/$(APP_NAME).$$arch \
	        app/DisplayArrangeApp.swift core/DisplayCore.c \
	        -framework AppKit -framework ServiceManagement \
	        -framework UserNotifications \
	        -framework CoreGraphics -framework CoreFoundation -framework IOKit || exit 1; \
	done
	lipo -create -output $(APP_DIR)/Contents/MacOS/$(APP_NAME) \
	    $(APP_DIR)/Contents/MacOS/$(APP_NAME).arm64 \
	    $(APP_DIR)/Contents/MacOS/$(APP_NAME).x86_64
	@rm $(APP_DIR)/Contents/MacOS/$(APP_NAME).arm64 \
	    $(APP_DIR)/Contents/MacOS/$(APP_NAME).x86_64
	@cp app/Info.plist $(APP_DIR)/Contents/
	@/usr/libexec/PlistBuddy -c "Set :CFBundleShortVersionString $(VERSION)" $(APP_DIR)/Contents/Info.plist
	@/usr/libexec/PlistBuddy -c "Set :CFBundleVersion $(VERSION)" $(APP_DIR)/Contents/Info.plist
	@cp assets/AppIcon.icns $(APP_DIR)/Contents/Resources/
	@cp scripts/install-update.sh $(APP_DIR)/Contents/Resources/
	@chmod +x $(APP_DIR)/Contents/Resources/install-update.sh
	@echo "Built $(APP_DIR) v$(VERSION)"

# ---------------------------------------------------------------- Tests
TEST_BIN := $(DIST_DIR)/da_characterization

test: $(TEST_BIN)
	./$(TEST_BIN)

$(TEST_BIN): tests/characterization.c $(CORE)
	@mkdir -p $(DIST_DIR)
	$(CC) $(CFLAGS) $(SDKFLAGS) -o $@ tests/characterization.c \
	    core/DisplayCore.c $(LDFLAGS)

# ---------------------------------------------------------------- Housekeeping
clean:
	rm -rf $(DIST_DIR)

install: cli
	mkdir -p $(DESTDIR)$(PREFIX)/bin
	cp $(DIST_DIR)/$(CLI_BIN) $(DESTDIR)$(PREFIX)/bin/

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/$(CLI_BIN)
