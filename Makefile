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

CC             := clang
ARCH           := arm64
SDK            := $(shell xcrun --sdk macosx --show-sdk-path)

CFLAGS         := -arch $(ARCH) -isysroot $(SDK) -mmacosx-version-min=12.0 \
                   -fPIC -O2 -Wall -Wextra -Wno-unused-parameter \
                   -std=gnu11 -fno-common -bundle

TOOL_CFLAGS    := -arch $(ARCH) -isysroot $(SDK) -mmacosx-version-min=12.0 \
                   -O2 -Wall -Wextra -Wno-unused-parameter -std=gnu11

FRAMEWORKS     := -framework CoreFoundation -framework CoreAudio -framework AudioToolbox

.PHONY: all clean sign verify install-info testtone

all: $(BUNDLE) verify testtone

testtone: $(TESTTONE_BIN)

$(TESTTONE_BIN): $(TESTTONE_SRC)
	@mkdir -p $(BUILD_DIR)
	$(CC) $(TOOL_CFLAGS) -o $(TESTTONE_BIN) $(TESTTONE_SRC) $(FRAMEWORKS)

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
