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
GUI_PRODUCT     := VAD
GUI_APP         := $(BUILD_DIR)/$(GUI_PRODUCT).app
GUI_CONTENTS    := $(GUI_APP)/Contents
GUI_MACOS_DIR   := $(GUI_CONTENTS)/MacOS
GUI_RESOURCES_DIR := $(GUI_CONTENTS)/Resources
GUI_EXECUTABLE  := $(GUI_MACOS_DIR)/$(GUI_PRODUCT)
GUI_INFO_PLIST_SRC := $(GUI_DIR)/Info.plist
GUI_BUILT_BIN   := $(GUI_DIR)/.build/release/$(GUI_PRODUCT)

GUI_ICON_SOURCE := $(GUI_DIR)/Resources/AppIconSource/icon-source.png
GUI_ICONSET_DIR := $(GUI_DIR)/Resources/AppIcon.iconset
GUI_ICNS        := $(GUI_DIR)/Resources/AppIcon.icns

INSTALLER_DIR            := installer
INSTALLER_STAGING        := $(BUILD_DIR)/pkg-root
INSTALLER_SCRIPTS_DIR    := $(INSTALLER_DIR)/scripts
INSTALLER_RESOURCES_DIR  := $(INSTALLER_DIR)/resources
INSTALLER_DISTRIBUTION   := $(INSTALLER_DIR)/distribution.xml
INSTALLER_COMPONENT_PKG  := $(BUILD_DIR)/VAD-component.pkg
INSTALLER_PKG            := $(BUILD_DIR)/RodeCasterVirtualAudio-Installer.pkg
INSTALLER_APP_IDENTIFIER := com.abrendt.rodecastervad.pkg.app
INSTALLER_VERSION        := 1.0.0

CC             := clang
ARCH           := arm64
SDK            := $(shell xcrun --sdk macosx --show-sdk-path)

CFLAGS         := -arch $(ARCH) -isysroot $(SDK) -mmacosx-version-min=12.0 \
                   -fPIC -O2 -Wall -Wextra -Wno-unused-parameter \
                   -std=gnu11 -fno-common -bundle

TOOL_CFLAGS    := -arch $(ARCH) -isysroot $(SDK) -mmacosx-version-min=12.0 \
                   -O2 -Wall -Wextra -Wno-unused-parameter -std=gnu11

FRAMEWORKS     := -framework CoreFoundation -framework CoreAudio -framework AudioToolbox

.PHONY: all everything clean sign verify install-info testtone gui gui-verify gui-icon daemon daemon-verify daemon-selftest installer

all: $(BUNDLE) verify testtone

# Combined target building every artifact this project produces: the HAL
# driver bundle, testtone, the router daemon, and the self-contained GUI
# app bundle (VAD.app, which embeds testtone + rodevad-router inside
# itself -- see the `gui` target below). This is what `make installer`
# (the .pkg build) runs first.
everything: all daemon gui

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

# Generates gui/RodeVADTester/Resources/AppIcon.icns from the single
# 1024x1024 source PNG using sips (Command Line Tools, no ImageMagick)
# plus iconutil. The generated .icns is committed to the repo (small,
# binary, same pattern as the other tracked Resources/*.plist files) so a
# fresh checkout doesn't require regenerating it just to build -- but this
# rule's dependency on GUI_ICON_SOURCE means `make` will still regenerate
# it automatically if the source PNG's mtime is newer than the committed
# .icns, e.g. after swapping in an updated icon design.
gui-icon: $(GUI_ICNS)

$(GUI_ICNS): $(GUI_ICON_SOURCE)
	@echo "--- generating AppIcon.icns from icon-source.png (sips + iconutil) ---"
	rm -rf $(GUI_ICONSET_DIR)
	mkdir -p $(GUI_ICONSET_DIR)
	sips -z 16 16     $(GUI_ICON_SOURCE) --out $(GUI_ICONSET_DIR)/icon_16x16.png      >/dev/null
	sips -z 32 32     $(GUI_ICON_SOURCE) --out $(GUI_ICONSET_DIR)/icon_16x16@2x.png   >/dev/null
	sips -z 32 32     $(GUI_ICON_SOURCE) --out $(GUI_ICONSET_DIR)/icon_32x32.png      >/dev/null
	sips -z 64 64     $(GUI_ICON_SOURCE) --out $(GUI_ICONSET_DIR)/icon_32x32@2x.png   >/dev/null
	sips -z 128 128   $(GUI_ICON_SOURCE) --out $(GUI_ICONSET_DIR)/icon_128x128.png    >/dev/null
	sips -z 256 256   $(GUI_ICON_SOURCE) --out $(GUI_ICONSET_DIR)/icon_128x128@2x.png >/dev/null
	sips -z 256 256   $(GUI_ICON_SOURCE) --out $(GUI_ICONSET_DIR)/icon_256x256.png    >/dev/null
	sips -z 512 512   $(GUI_ICON_SOURCE) --out $(GUI_ICONSET_DIR)/icon_256x256@2x.png >/dev/null
	sips -z 512 512   $(GUI_ICON_SOURCE) --out $(GUI_ICONSET_DIR)/icon_512x512.png    >/dev/null
	sips -z 1024 1024 $(GUI_ICON_SOURCE) --out $(GUI_ICONSET_DIR)/icon_512x512@2x.png >/dev/null
	iconutil -c icns $(GUI_ICONSET_DIR) -o $(GUI_ICNS)
	rm -rf $(GUI_ICONSET_DIR)

# Builds the SwiftUI channel-tester GUI app via `swift build` (no Xcode
# required -- Swift Package Manager + the Command Line Tools are
# sufficient), then hand-assembles a minimal .app bundle around the
# resulting executable, matching the same Info.plist / Contents/MacOS
# layout convention used by the HAL driver bundle above.
#
# The bundle is self-contained: testtone and rodevad-router are copied
# INSIDE it (Contents/MacOS/testtone, Contents/MacOS/rodevad-router) in
# addition to staying as loose build/testtone and build/rodevad-router
# binaries (still useful for the CLI-only dev workflow, e.g. running
# `./build/testtone --list` directly). This is what makes
# build/VAD.app relocatable to /Applications (installed by the `installer`
# .pkg target below) without needing its helper binaries to sit next to it
# as loose sibling files -- see ProjectLayout.swift's resolution order
# (inside-bundle first, then sibling-of-bundle, then the dev-mode
# CWD-walk fallback).
gui: testtone daemon gui-icon
	@echo "--- swift build (release, arm64) ---"
	cd $(GUI_DIR) && swift build -c release --arch arm64
	@mkdir -p $(GUI_MACOS_DIR)
	@mkdir -p $(GUI_RESOURCES_DIR)
	cp $(GUI_BUILT_BIN) $(GUI_EXECUTABLE)
	cp $(GUI_INFO_PLIST_SRC) $(GUI_CONTENTS)/Info.plist
	cp $(GUI_ICNS) $(GUI_RESOURCES_DIR)/AppIcon.icns
	@echo "--- embedding testtone + rodevad-router into Contents/MacOS/ (self-contained bundle) ---"
	cp $(TESTTONE_BIN) $(GUI_MACOS_DIR)/testtone
	cp $(DAEMON_BIN) $(GUI_MACOS_DIR)/rodevad-router
	$(MAKE) gui-verify

gui-verify:
	@echo "--- plutil -lint GUI Info.plist ---"
	plutil -lint $(GUI_CONTENTS)/Info.plist
	@echo "--- verify embedded testtone + rodevad-router are present and executable ---"
	test -x $(GUI_MACOS_DIR)/testtone && test -x $(GUI_MACOS_DIR)/rodevad-router && \
		echo "OK: Contents/MacOS/testtone and Contents/MacOS/rodevad-router are present and executable" || \
		(echo "FAIL: embedded testtone/rodevad-router missing or not executable" && exit 1)
	@echo "--- verify AppIcon.icns is present and valid ---"
	test -f $(GUI_RESOURCES_DIR)/AppIcon.icns && \
		file $(GUI_RESOURCES_DIR)/AppIcon.icns | grep -q "Mac OS X icon" && \
		echo "OK: Contents/Resources/AppIcon.icns present and recognized as a Mac OS X icon" || \
		(echo "FAIL: Contents/Resources/AppIcon.icns missing or not a valid icns" && exit 1)
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

# Builds a real, double-clickable macOS installer package
# (build/RodeCasterVirtualAudio-Installer.pkg) using pkgbuild + productbuild
# (both ship with macOS itself -- no Xcode.app needed). This is the
# proper, native way to install the whole system in one step: the driver
# to /Library/Audio/Plug-Ins/HAL/, VAD.app to /Applications, and the
# router daemon as a per-user LaunchAgent for whoever is logged in when
# the installer runs -- see installer/scripts/postinstall for exactly
# what the (root-context) postinstall script does.
#
# The payload root mirrors the final filesystem layout directly
# (Applications/VAD.app), plus a hidden .rodecaster-payload/ staging
# directory the postinstall script consumes and deletes (holding the
# driver bundle and the LaunchAgent plist template, since a pkgbuild
# payload can only place files, not decide *where* per-user files should
# go until the postinstall script runs as root and can determine the
# actual console user).
#
# This is build+verify only -- `make installer` never runs/installs the
# resulting .pkg. See README "Installer (.pkg)" for the manual, deliberate
# double-click-to-install step and its safety notes.
installer: everything
	@echo "--- staging installer payload ---"
	rm -rf $(INSTALLER_STAGING)
	mkdir -p $(INSTALLER_STAGING)/.rodecaster-payload
	cp -R $(GUI_APP) $(INSTALLER_STAGING)/$(GUI_PRODUCT).app
	cp -R $(BUNDLE) $(INSTALLER_STAGING)/.rodecaster-payload/$(PRODUCT_NAME).driver
	cp daemon/com.abrendt.rodevad.router.plist $(INSTALLER_STAGING)/.rodecaster-payload/com.abrendt.rodevad.router.plist
	@echo "--- building component package (pkgbuild) ---"
	pkgbuild --root $(INSTALLER_STAGING) \
	         --install-location /Applications \
	         --identifier $(INSTALLER_APP_IDENTIFIER) \
	         --version $(INSTALLER_VERSION) \
	         --scripts $(INSTALLER_SCRIPTS_DIR) \
	         $(INSTALLER_COMPONENT_PKG)
	@echo "--- building distribution package (productbuild) ---"
	productbuild --distribution $(INSTALLER_DISTRIBUTION) \
	             --package-path $(BUILD_DIR) \
	             --resources $(INSTALLER_RESOURCES_DIR) \
	             $(INSTALLER_PKG)
	@echo "--- pkgutil --check-signature (expected: unsigned/ad-hoc, not an error) ---"
	-pkgutil --check-signature $(INSTALLER_PKG)
	@echo "OK: built $(INSTALLER_PKG)"

clean:
	rm -rf $(BUILD_DIR)
