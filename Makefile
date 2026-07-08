# Compiler
CXX ?= g++
CC ?= gcc
PKG_CONFIG ?= pkg-config
HOST_XXD ?= xxd
INSTALL ?= install
PREFIX ?= /usr
SYSCONFDIR ?= /etc
SRC_DIR := ./src
OUT_DIR := ./out
RES_DIR := $(SRC_DIR)/resource
GEN_DIR := $(SRC_DIR)/autogen
BUILD_DIR := $(OUT_DIR)/$(BUILD_TYPE)

# File lists
SRCS := $(shell find $(SRC_DIR) -type f -name '*.cpp')
OBJS=$(patsubst $(SRC_DIR)/%.cpp,$(BUILD_DIR)/%.o,$(SRCS))

# C sources: the vendored nanopb runtime and the generated AA protobuf code.
SRCS_C := $(shell find $(SRC_DIR) -type f -name '*.c')
OBJS += $(patsubst $(SRC_DIR)/%.c,$(BUILD_DIR)/%.c.o,$(SRCS_C))

RES := $(shell find $(RES_DIR) -type f ! -name '*.h' ! -name '.*' -name '*.*')
RES_SRC := $(patsubst $(RES_DIR)/%,$(GEN_DIR)/%.cpp,$(RES))

# Targets
TARGET_NAME := app

# Build types
.PHONY: all debug release clean build

all: release

LDOPTIONS := $(shell $(PKG_CONFIG) --libs sdl2 SDL2_ttf libavformat libavcodec libavutil libswscale libusb-1.0 openssl) -pthread
LDFLAGS :=
CXXCOMMON := -Wall -std=c++17 -Isrc -Isrc/protocol/aa/nanopb -Isrc/protocol/aa/proto
CCOMMON := -Wall -std=c99 -Isrc/protocol/aa/nanopb -Isrc/protocol/aa/proto

# Allwinner Cedar hardware H.264 decode (F1C200s). Enable with USE_CEDAR=1;
# libcedarc headers come from the (cross) sysroot, libs are linked here. libdrm
# is for the optional DE backend UYVY plane (renderer = drm).
ifeq ($(USE_CEDAR),1)
CXXCOMMON += -DUSE_CEDAR $(shell $(PKG_CONFIG) --cflags libdrm)
# -latomic: the 32-bit ARMv5 (arm926) target has no native 64-bit atomics, so
# std::atomic<int64_t> in the Android Auto backend needs libatomic.
LDOPTIONS += -lvdecoder -lcdc_base -lMemAdapter -lVE -lvideoengine -ldl -lrt -latomic $(shell $(PKG_CONFIG) --libs libdrm)
endif

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

release: BUILD_TYPE := release
release: CXXFLAGS ?= -O2 -ffast-math -fno-rtti -fdata-sections -ffunction-sections -fomit-frame-pointer -fvisibility=hidden -pipe -DNDEBUG
release: LDFLAGS += -O2 -ffast-math -Wl,-O1 $(PLATFORM_LDFLAGS)
release: TARGET := $(TARGET_NAME)
release: prepare

prepare: $(RES_SRC)
	$(MAKE) BUILD_TYPE=$(BUILD_TYPE) TARGET=$(OUT_DIR)/$(TARGET) CXXFLAGS="$(CXXFLAGS)" LDFLAGS="$(LDFLAGS)" build

build: $(TARGET)

$(GEN_DIR)/%.cpp: $(RES_DIR)/%
	@mkdir -p $(GEN_DIR)
	$(HOST_XXD) -i -n $(basename $(notdir $<)) $< > $@

$(TARGET): $(OBJS)
	@mkdir -p $(OUT_DIR)
	$(CXX) $(LDFLAGS) $(OBJS) -o $(TARGET) $(LDOPTIONS)
	@echo "Build complete: $(TARGET)"

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXCOMMON) $(shell $(PKG_CONFIG) --cflags sdl2 SDL2_ttf libavformat libavcodec libavutil libswscale libusb-1.0 openssl) $(CXXFLAGS) -c $< -o $@

# C rule for nanopb + generated protobuf code (-fno-rtti is C++-only).
$(BUILD_DIR)/%.c.o: $(SRC_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CCOMMON) $(filter-out -fno-rtti,$(CXXFLAGS)) -c $< -o $@

install:
	$(INSTALL) -D -m 0755 $(OUT_DIR)/$(TARGET_NAME) $(DESTDIR)$(PREFIX)/bin/fastcarplay
	$(INSTALL) -D -m 0644 settings.txt $(DESTDIR)$(SYSCONFDIR)/fastcarplay/settings.txt

clean:
	@rm -rf $(OUT_DIR)
	@rm -rf $(GEN_DIR)
	@echo "Clean complete"
