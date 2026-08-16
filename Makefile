# Makefile for RodeCasterVirtualAudio.driver
#
# Builds a CoreAudio HAL plug-in bundle (AudioServerPlugIn) for arm64
# macOS. Produces build/RodeCasterVirtualAudio.driver.

PRODUCT_NAME   := RodeCasterVirtualAudio
BUNDLE_ID      := com.abrendt.rodecastervad
BUILD_DIR      := build
BUNDLE         := $(BUILD_DIR)/$(PRODUCT_NAME).driver
CONTENTS       := $(BUNDLE)/Contents
MACOS_DIR      := $(CONTENTS)/MacOS
EXECUTABLE     := $(MACOS_DIR)/$(PRODUCT_NAME)

SRC            := src/RodeCasterVirtualAudio.c
INFO_PLIST_SRC := Resources/Info.plist
VERSION_PLIST_SRC := Resources/version.plist

TESTTONE_SRC   := tools/testtone.c
TESTTONE_BIN   := $(BUILD_DIR)/testtone

DAEMON_SRC      := daemon/rodevad-router.c
DAEMON_BIN      := $(BUILD_DIR)/rodevad-router

GUI_DIR         := gui/RodeVADTester
GUI_PRODUCT     := RodeVADTester
GUI_APP         := $(BUILD_DIR)/$(GUI_PRODUCT).app
GUI_CONTENTS    := $(GUI_APP)/Contents
GUI_MACOS_DIR   := $(GUI_CONTENTS)/MacOS
GUI_EXECUTABLE  := $(GUI_MACOS_DIR)/$(GUI_PRODUCT)
GUI_INFO_PLIST_SRC := $(GUI_DIR)/Info.plist
GUI_BUILT_BIN   := $(GUI_DIR)/.build/release/$(GUI_PRODUCT)

CC             := clang
ARCH           := arm64
SDK            := $(shell xcrun --sdk macosx --show-sdk-path)

CFLAGS         := -arch $(ARCH) -isysroot $(SDK) -mmacosx-version-min=12.0 \
                   -fPIC -O2 -Wall -Wextra -Wno-unused-parameter \
                   -std=gnu11 -fno-common -bundle

TOOL_CFLAGS    := -arch $(ARCH) -isysroot $(SDK) -mmacosx-version-min=12.0 \
                   -O2 -Wall -Wextra -Wno-unused-parameter -std=gnu11

FRAMEWORKS     := -framework CoreFoundation -framework CoreAudio -framework AudioToolbox

.PHONY: all clean sign verify install-info testtone gui gui-verify daemon daemon-verify daemon-selftest

all: $(BUNDLE) verify testtone

testtone: $(TESTTONE_BIN)

$(TESTTONE_BIN): $(TESTTONE_SRC)
	@mkdir -p $(BUILD_DIR)
	$(CC) $(TOOL_CFLAGS) -o $(TESTTONE_BIN) $(TESTTONE_SRC) $(FRAMEWORKS)

# Builds the standalone routing daemon (daemon/rodevad-router.c). This is
# an ordinary command-line binary, NOT part of the HAL plug-in bundle --
# it is meant to run as a per-user LaunchAgent, bridging the 5 virtual
# devices into the real RodeCaster Pro 2 hardware. See README "Routing
# daemon" for what it does and the manual steps to install/run it.
daemon: $(DAEMON_BIN)
	$(MAKE) daemon-verify

$(DAEMON_BIN): $(DAEMON_SRC)
	@mkdir -p $(BUILD_DIR)
	$(CC) $(TOOL_CFLAGS) -o $(DAEMON_BIN) $(DAEMON_SRC) $(FRAMEWORKS)

daemon-verify: $(DAEMON_BIN)
	@echo "--- codesign (ad-hoc) rodevad-router ---"
	codesign --force --sign - $(DAEMON_BIN)
	codesign -dv $(DAEMON_BIN)
	@echo "--- offline self-test (pure buffer math, no device IO) ---"
	$(DAEMON_BIN) --selftest

# Convenience alias: just the offline self-test, no (re)build forced.
daemon-selftest: $(DAEMON_BIN)
	$(DAEMON_BIN) --selftest

# Builds the SwiftUI channel-tester GUI app via `swift build` (no Xcode
# required -- Swift Package Manager + the Command Line Tools are
# sufficient), then hand-assembles a minimal .app bundle around the
# resulting executable, matching the same Info.plist / Contents/MacOS
# layout convention used by the HAL driver bundle above.
gui: testtone
	@echo "--- swift build (release, arm64) ---"
	cd $(GUI_DIR) && swift build -c release --arch arm64
	@mkdir -p $(GUI_MACOS_DIR)
	cp $(GUI_BUILT_BIN) $(GUI_EXECUTABLE)
	cp $(GUI_INFO_PLIST_SRC) $(GUI_CONTENTS)/Info.plist
	$(MAKE) gui-verify

gui-verify:
	@echo "--- plutil -lint GUI Info.plist ---"
	plutil -lint $(GUI_CONTENTS)/Info.plist
	@echo "--- codesign (ad-hoc) GUI app ---"
	codesign --force --deep --sign - $(GUI_APP)
	codesign -dv $(GUI_APP)
	@echo "--- otool: confirm SwiftUI/AppKit linkage ---"
	otool -L $(GUI_EXECUTABLE) | grep -qE 'SwiftUI|AppKit' && \
		echo "OK: GUI executable links SwiftUI/AppKit" || \
		(echo "FAIL: GUI executable does not link SwiftUI/AppKit" && exit 1)

$(EXECUTABLE): $(SRC)
	@mkdir -p $(MACOS_DIR)
	$(CC) $(CFLAGS) -o $(EXECUTABLE) $(SRC) $(FRAMEWORKS)

$(CONTENTS)/Info.plist: $(INFO_PLIST_SRC)
	@mkdir -p $(CONTENTS)
	cp $(INFO_PLIST_SRC) $(CONTENTS)/Info.plist

$(CONTENTS)/version.plist: $(VERSION_PLIST_SRC)
	@mkdir -p $(CONTENTS)
	cp $(VERSION_PLIST_SRC) $(CONTENTS)/version.plist

$(BUNDLE): $(EXECUTABLE) $(CONTENTS)/Info.plist $(CONTENTS)/version.plist
	@touch $(BUNDLE)

sign: $(BUNDLE)
	codesign --force --deep --sign - $(BUNDLE)

verify: $(BUNDLE)
	@echo "--- plutil -lint Info.plist ---"
	plutil -lint $(CONTENTS)/Info.plist
	@echo "--- codesign (ad-hoc) ---"
	codesign --force --deep --sign - $(BUNDLE)
	codesign -dv $(BUNDLE)
	@echo "--- nm: factory entry point export check ---"
	nm -gU $(EXECUTABLE) | grep -q RodeCasterVirtualAudio_Factory && \
		echo "OK: RodeCasterVirtualAudio_Factory is exported" || \
		(echo "FAIL: factory entry point not exported" && exit 1)

clean:
	rm -rf $(BUILD_DIR)
