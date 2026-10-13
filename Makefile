# Compiler
CXX ?= g++
CC ?= gcc
PKG_CONFIG ?= pkg-config
INSTALL ?= install
STRIP ?= strip
PREFIX ?= /usr
SYSCONFDIR ?= /etc
SRC_DIR := ./src
OUT_DIR := ./out
GEN_DIR := $(SRC_DIR)/autogen
BUILD_DIR := $(OUT_DIR)/$(BUILD_TYPE)

# File lists. src/ui (the LVGL UI) is added
# back only when USE_LVGL=1, so a UI-less build pays no flash/RAM for it.
SRCS := $(shell find $(SRC_DIR) -type f -name '*.cpp' -not -path '$(SRC_DIR)/ui/*')
OBJS=$(patsubst $(SRC_DIR)/%.cpp,$(BUILD_DIR)/%.o,$(SRCS))

# C sources: the vendored nanopb runtime and the generated AA protobuf code.
SRCS_C := $(shell find $(SRC_DIR) -type f -name '*.c' -not -path '$(SRC_DIR)/ui/*')
OBJS += $(patsubst $(SRC_DIR)/%.c,$(BUILD_DIR)/%.c.o,$(SRCS_C))

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

# opus: the fixed-point Opus encoder for the wireless CarPlay mic uplink (decode
# still rides libavcodec's libopus wrapper). <opus/opus.h> resolves via the
# sysroot's default include path, so only the library (-lopus) is added here.
LDOPTIONS := $(shell $(PKG_CONFIG) --libs libavformat libavcodec libavutil libswscale libusb-1.0 openssl opus alsa) -pthread
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
ifeq ($(USE_LVGL),1)
LVGL_DIR := ./third_party/lvgl
LVGL_SRCS := $(shell find $(LVGL_DIR)/src -type f -name '*.c')
LVGL_OBJS := $(patsubst $(LVGL_DIR)/%.c,$(BUILD_DIR)/lvgl/%.c.o,$(LVGL_SRCS))
OBJS += $(LVGL_OBJS)
# LV_CONF_INCLUDE_SIMPLE: LVGL picks up src/ui/lv_conf.h from the include path.
UI_FLAGS := -DUSE_LVGL -DLV_CONF_INCLUDE_SIMPLE -I$(SRC_DIR)/ui -Ithird_party
SRCS += $(shell find $(SRC_DIR)/ui -maxdepth 1 -type f -name '*.cpp')
SRCS_C += $(shell find $(SRC_DIR)/ui -maxdepth 1 -type f -name '*.c')
CXXCOMMON += $(UI_FLAGS)
CCOMMON += $(UI_FLAGS)
endif

# On-device UI, Slint flavor: links the static libslint_cpp built by the
# buildroot slint-cpp package (software renderer, fonts embedded at compile
# time). Screens are declarative (src/ui/slint/app.slint); the only C++20 TU
# is the facade, src/ui/slint_osd.cpp. Enable USE_SLINT=1.
ifeq ($(USE_SLINT),1)
ifeq ($(USE_LVGL),1)
$(error USE_LVGL and USE_SLINT are mutually exclusive)
endif
# Headers + libslint_cpp.a come from the cross sysroot (the slint-cpp package
# installs into staging); slint-compiler from the buildroot host tools.
SLINT_SYSROOT ?= $(shell $(CXX) -print-sysroot)
SLINT_COMPILER ?= slint-compiler
SLINT_GEN := $(GEN_DIR)/app_window.h
UI_FLAGS := -DUSE_SLINT -I$(SRC_DIR)/ui -I$(GEN_DIR) -I$(SLINT_SYSROOT)/usr/include/slint
SRCS += $(SRC_DIR)/ui/slint_osd.cpp $(SRC_DIR)/ui/ui_bridge.cpp
CXXCOMMON += $(UI_FLAGS)
LDOPTIONS += -lslint_cpp

# Slint's C++ API needs C++20; only the facade sees those headers, the rest
# of the tree stays C++17.
$(BUILD_DIR)/ui/slint_osd.o: CXXCOMMON := $(filter-out -std=c++17,$(CXXCOMMON)) -std=c++20
$(BUILD_DIR)/ui/slint_osd.o: $(SLINT_GEN)

$(SLINT_GEN): $(SRC_DIR)/ui/slint/app.slint $(wildcard $(SRC_DIR)/ui/slint/fonts/*)
	@mkdir -p $(GEN_DIR)
	$(SLINT_COMPILER) -f cpp --embed-resources=embed-for-software-renderer -o $@ $<
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

prepare: version-h
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

$(TARGET): $(OBJS)
	@mkdir -p $(OUT_DIR)
	$(CXX) $(LDFLAGS) $(OBJS) -o $(TARGET) $(LDOPTIONS)
	@echo "Build complete: $(TARGET)"

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXCOMMON) $(shell $(PKG_CONFIG) --cflags libavformat libavcodec libavutil libswscale libusb-1.0 openssl alsa) $(CXXFLAGS) -MMD -MP -c $< -o $@

# C rule for nanopb + generated protobuf code (-fno-rtti is C++-only).
$(BUILD_DIR)/%.c.o: $(SRC_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CCOMMON) $(filter-out -fno-rtti,$(CXXFLAGS)) -MMD -MP -c $< -o $@

# Vendored LVGL. Third-party: warnings off, and it never sees our -Wall/-Werror.
$(BUILD_DIR)/lvgl/%.c.o: $(LVGL_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) -std=gnu99 -w -DLV_CONF_INCLUDE_SIMPLE -I$(SRC_DIR)/ui -I$(LVGL_DIR) \
		$(filter-out -fno-rtti,$(CXXFLAGS)) -c $< -o $@

# Header dependencies (-MMD): editing a header now rebuilds what includes it.
# NOTE: this does NOT cover changing the feature flags themselves -- toggling
# USE_LVGL / USE_SLINT / USE_CEDRUS / USE_AA_WIRELESS changes the compile
# flags, not the sources, so make cannot see it. Always `make clean` when
# switching a flag, or you get a binary built from a mix of both configurations.
-include $(OBJS:.o=.d)

install:
	$(INSTALL) -D -m 0755 -s --strip-program=$(STRIP) $(OUT_DIR)/$(TARGET_NAME) $(DESTDIR)$(PREFIX)/bin/fastcarplay
	$(INSTALL) -D -m 0644 settings.txt $(DESTDIR)$(SYSCONFDIR)/fastcarplay/settings.txt

clean:
	@rm -rf $(OUT_DIR)
	@rm -rf $(GEN_DIR)
	@echo "Clean complete"
