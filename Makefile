# Compiler
CXX ?= g++
CC ?= gcc
PKG_CONFIG ?= pkg-config
HOST_XXD ?= xxd
INSTALL ?= install
STRIP ?= strip
PREFIX ?= /usr
SYSCONFDIR ?= /etc
SRC_DIR := ./src
OUT_DIR := ./out
RES_DIR := $(SRC_DIR)/resource
GEN_DIR := $(SRC_DIR)/autogen
BUILD_DIR := $(OUT_DIR)/$(BUILD_TYPE)

# File lists. src/ui (LVGL backend + EEZ Studio generated screens) is added
# back only when USE_LVGL=1, so a UI-less build pays no flash/RAM for it.
SRCS := $(shell find $(SRC_DIR) -type f -name '*.cpp' -not -path '$(SRC_DIR)/ui/*')
OBJS=$(patsubst $(SRC_DIR)/%.cpp,$(BUILD_DIR)/%.o,$(SRCS))

# C sources: the vendored nanopb runtime and the generated AA protobuf code.
SRCS_C := $(shell find $(SRC_DIR) -type f -name '*.c' -not -path '$(SRC_DIR)/ui/*')
OBJS += $(patsubst $(SRC_DIR)/%.c,$(BUILD_DIR)/%.c.o,$(SRCS_C))

RES := $(shell find $(RES_DIR) -type f ! -name '*.h' ! -name '.*' -name '*.*')
RES_SRC := $(patsubst $(RES_DIR)/%,$(GEN_DIR)/%.cpp,$(RES))

# Version identity, compiled in so the running binary can be asked what it is --
# which after a partial update is not the same as what /etc claims.
# A dev checkout has git; the buildroot package builds from a GitHub tarball
# with no .git, so it passes these in instead. FCP_BUILD is a plain monotonic
# integer: the device has no RTC, so nothing may be derived from a date.
FCP_VERSION ?= $(shell git describe --tags --always --dirty 2>/dev/null || echo unknown)
FCP_BUILD ?= 0
VERSION_H := $(GEN_DIR)/version.h

# Targets
TARGET_NAME := fastcarplay

# Build types
.PHONY: all debug release clean build

all: release

LDOPTIONS := $(shell $(PKG_CONFIG) --libs sdl2 SDL2_ttf libavformat libavcodec libavutil libswscale libusb-1.0 openssl) -pthread
LDFLAGS :=
CXXCOMMON := -Wall -std=c++17 -Isrc -Isrc/protocol/aa/nanopb -Isrc/protocol/aa/proto
CCOMMON := -Wall -std=c99 -Isrc/protocol/aa/nanopb -Isrc/protocol/aa/proto

# Mainline cedrus HW H.264 decode via ffmpeg's V4L2-Request hwaccel (F1C200s),
# blob-free: no libcedarc, only libdrm + libav* (already linked). Enable USE_CEDRUS=1.
ifeq ($(USE_CEDRUS),1)
CXXCOMMON += -DUSE_CEDRUS $(shell $(PKG_CONFIG) --cflags libdrm)
# -latomic: 64-bit atomics in the Android Auto backend on 32-bit arm926.
LDOPTIONS += -lrt -latomic $(shell $(PKG_CONFIG) --libs libdrm)
endif

# Wireless Android Auto (Bluetooth bootstrap + Wi-Fi AP). Enable USE_AA_WIRELESS=1
# once the target image has dbus + bluez (dev in the cross sysroot) and, at
# runtime, hostapd + dnsmasq. Without it, protocol = aa-wireless is unavailable.
ifeq ($(USE_AA_WIRELESS),1)
CXXCOMMON += -DUSE_AA_WIRELESS $(shell $(PKG_CONFIG) --cflags dbus-1)
LDOPTIONS += $(shell $(PKG_CONFIG) --libs dbus-1) -lbluetooth
endif

# Wireless CarPlay (protocol = carplay-wireless): the iPhone bootstrap over
# Bluetooth (BlueZ/dbus) + Wi-Fi AP handoff, then the :7000 control server feeds
# decoded HEVC screen video into the app. Enable USE_CP_WIRELESS=1 (needs dbus +
# bluez, and at runtime hostapd + dnsmasq + the MFi auth chip on i2c).
ifeq ($(USE_CP_WIRELESS),1)
CXXCOMMON += -DUSE_CP_WIRELESS $(shell $(PKG_CONFIG) --cflags dbus-1)
LDOPTIONS += $(shell $(PKG_CONFIG) --libs dbus-1)
endif

# Wired CarPlay (protocol = carplay-wired): bring the USB-plugged iPhone to
# config 6 and open com.apple.carkit.service via our own usbmux (cp_usbmux) +
# libimobiledevice; the phone then streams CarPlay over the USB-NCM link to the
# :7000 server. Enable USE_CP_WIRED=1 (needs libimobiledevice + libplist; at
# runtime the MFi auth chip on i2c and root for usbfs/sysfs).
ifeq ($(USE_CP_WIRED),1)
CXXCOMMON += -DUSE_CP_WIRED $(shell $(PKG_CONFIG) --cflags libimobiledevice-1.0 libplist-2.0)
LDOPTIONS += $(shell $(PKG_CONFIG) --libs libimobiledevice-1.0 libplist-2.0)
endif

# On-device UI: LVGL 9.3.0 (MIT), vendored source in third_party/lvgl (see
# its VENDORING.md), compiled in. Screens live in src/ui. Enable USE_LVGL=1.
# By default only the hand-written picker screens are built. The EEZ Studio
# generated flow engine (src/ui/generated, ~320 KB of .text + 12 KB .bss) is
# dead unless lvgl-screen=generated is selected at runtime, so it is compiled
# in only with USE_EEZ=1 -- a UI-less-of-it build pays nothing for it.
ifeq ($(USE_LVGL),1)
LVGL_DIR := ./third_party/lvgl
LVGL_SRCS := $(shell find $(LVGL_DIR)/src -type f -name '*.c')
LVGL_OBJS := $(patsubst $(LVGL_DIR)/%.c,$(BUILD_DIR)/lvgl/%.c.o,$(LVGL_SRCS))
OBJS += $(LVGL_OBJS)
# LV_CONF_INCLUDE_SIMPLE: LVGL picks up src/ui/lv_conf.h from the include path.
UI_FLAGS := -DUSE_LVGL -DLV_CONF_INCLUDE_SIMPLE -I$(SRC_DIR)/ui -Ithird_party
ifeq ($(USE_EEZ),1)
# The whole src/ui tree, including the generated EEZ flow screens.
SRCS += $(shell find $(SRC_DIR)/ui -type f -name '*.cpp')
SRCS_C += $(shell find $(SRC_DIR)/ui -type f -name '*.c')
# -Ithird_party already added; the generated code includes <lvgl/lvgl.h>.
UI_FLAGS += -DUSE_EEZ -I$(SRC_DIR)/ui/generated
# The EEZ flow engine is a generic LVGL runtime that references the full
# widget set, so lv_conf.h must enable them -- and the vendored LVGL TUs below
# need the same define to actually compile those widgets in.
LVGL_CONF_FLAGS := -DUSE_EEZ
else
# Only the hand-written UI (everything under src/ui except generated/).
SRCS += $(shell find $(SRC_DIR)/ui -maxdepth 1 -type f -name '*.cpp')
SRCS_C += $(shell find $(SRC_DIR)/ui -maxdepth 1 -type f -name '*.c')
endif
CXXCOMMON += $(UI_FLAGS)
CCOMMON += $(UI_FLAGS)
endif

debug: BUILD_TYPE := debug
debug: CXXFLAGS := -g -O0 -DPROTOCOL_DEBUG -fsanitize=address -fno-omit-frame-pointer
debug: LDFLAGS += -fsanitize=address -fno-omit-frame-pointer
debug: TARGET := $(TARGET_NAME)-debug
debug: prepare

ifeq ($(shell uname -s), Darwin)
    # macOS / clang
    PLATFORM_LDFLAGS :=
else
    # Linux / GCC (cross or native)
    PLATFORM_LDFLAGS := -Wl,--gc-sections -Wl,--as-needed
endif

# -ffast-math is deliberately absent: the target is soft-float (no FPU), so
# relaxing IEEE rules cannot speed up a libgcc call, and its -ffinite-math-only
# would quietly break isnan/isinf across the ffmpeg/SDL headers inlined here.
# -Os over -O2: .text is ~2 MB against a 16 KB I-cache, so smaller code wins
# the cache more than -O2's inlining loses. -flto lets --gc-sections reach
# cross-TU dead code (much of the unused LVGL surface).
release: BUILD_TYPE := release
release: CXXFLAGS ?= -Os -flto -fno-rtti -fdata-sections -ffunction-sections -fomit-frame-pointer -fvisibility=hidden -pipe -DNDEBUG
release: LDFLAGS += -Os -flto -Wl,-O1 $(PLATFORM_LDFLAGS)
release: TARGET := $(TARGET_NAME)
release: prepare

prepare: $(RES_SRC) version-h
	$(MAKE) BUILD_TYPE=$(BUILD_TYPE) TARGET=$(OUT_DIR)/$(TARGET) CXXFLAGS="$(CXXFLAGS)" LDFLAGS="$(LDFLAGS)" build

build: $(TARGET)

# Rewritten only when the content actually changes, so a dirty working tree
# does not force a full rebuild on every make.
.PHONY: version-h
version-h:
	@mkdir -p $(GEN_DIR)
	@printf '// Generated by the Makefile. Do not edit.\n#pragma once\n#define FCP_VERSION "%s"\n#define FCP_BUILD %s\n' \
		'$(FCP_VERSION)' '$(FCP_BUILD)' > $(VERSION_H).new
	@if cmp -s $(VERSION_H).new $(VERSION_H) 2>/dev/null; then rm -f $(VERSION_H).new; \
	else mv $(VERSION_H).new $(VERSION_H); echo "version: $(FCP_VERSION) build $(FCP_BUILD)"; fi

# Embed resources as `const` arrays so they land in .rodata (shared, clean,
# read-only) instead of the writable .data segment -- xxd -i emits a plain
# `unsigned char`, which for the ~640 KB of background+font would otherwise
# sit in a dirty-able RW mapping.
$(GEN_DIR)/%.cpp: $(RES_DIR)/%
	@mkdir -p $(GEN_DIR)
	$(HOST_XXD) -i -n $(basename $(notdir $<)) $< | \
	  sed -e 's/^unsigned char /extern const unsigned char /' \
	      -e 's/^unsigned int /extern const unsigned int /' > $@

$(TARGET): $(OBJS)
	@mkdir -p $(OUT_DIR)
	$(CXX) $(LDFLAGS) $(OBJS) -o $(TARGET) $(LDOPTIONS)
	@echo "Build complete: $(TARGET)"

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXCOMMON) $(shell $(PKG_CONFIG) --cflags sdl2 SDL2_ttf libavformat libavcodec libavutil libswscale libusb-1.0 openssl) $(CXXFLAGS) -MMD -MP -c $< -o $@

# C rule for nanopb + generated protobuf code (-fno-rtti is C++-only).
$(BUILD_DIR)/%.c.o: $(SRC_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CCOMMON) $(filter-out -fno-rtti,$(CXXFLAGS)) -MMD -MP -c $< -o $@

# Vendored LVGL. Third-party: warnings off, and it never sees our -Wall/-Werror.
# LVGL_CONF_FLAGS carries only the lv_conf.h feature toggles it must agree with
# the rest of the build on (e.g. -DUSE_EEZ, which enables the full widget set).
$(BUILD_DIR)/lvgl/%.c.o: $(LVGL_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) -std=gnu99 -w -DLV_CONF_INCLUDE_SIMPLE -I$(SRC_DIR)/ui -I$(LVGL_DIR) \
		$(LVGL_CONF_FLAGS) $(filter-out -fno-rtti,$(CXXFLAGS)) -c $< -o $@

# Header dependencies (-MMD): editing a header now rebuilds what includes it.
# NOTE: this does NOT cover changing the feature flags themselves -- toggling
# USE_LVGL / USE_CEDRUS / USE_AA_WIRELESS changes the compile flags, not the
# sources, so make cannot see it. Always `make clean` when switching a flag,
# or you get a binary built from a mix of both configurations.
-include $(OBJS:.o=.d)

install:
	$(INSTALL) -D -m 0755 -s --strip-program=$(STRIP) $(OUT_DIR)/$(TARGET_NAME) $(DESTDIR)$(PREFIX)/bin/fastcarplay
	$(INSTALL) -D -m 0644 settings.txt $(DESTDIR)$(SYSCONFDIR)/fastcarplay/settings.txt

clean:
	@rm -rf $(OUT_DIR)
	@rm -rf $(GEN_DIR)
	@echo "Clean complete"
