# Makefile for compiling buttons.cpp with optimizations for release

CXX := g++
CXXFLAGS := -std=c++17 -O3 -Wall -Wextra -march=native -flto
LDFLAGS := -flto
OUT_DIR := ./../out

all: keylistener emulator

# Round-trip self-test for the vendored nanopb + generated AA protobuf code.
AA_DIR := ../src/protocol/aa
aa_proto_test: aa_proto_test.cpp
	$(CXX) $(CXXFLAGS) -I$(AA_DIR)/nanopb -I$(AA_DIR)/proto aa_proto_test.cpp \
	  $(AA_DIR)/nanopb/pb_common.c $(AA_DIR)/nanopb/pb_encode.c $(AA_DIR)/nanopb/pb_decode.c \
	  $(shell find $(AA_DIR)/proto -name '*.pb.c') -o $(OUT_DIR)/aa_proto_test $(LDFLAGS)

# Round-trip check for the multitouch InputReport builder (links aa_proto).
aa_mt_test: aa_mt_test.cpp
	$(CXX) $(CXXFLAGS) -I../src -I$(AA_DIR)/nanopb -I$(AA_DIR)/proto aa_mt_test.cpp \
	  $(AA_DIR)/aa_proto.cpp ../src/settings.cpp ../src/common/logger.cpp \
	  $(AA_DIR)/nanopb/pb_common.c $(AA_DIR)/nanopb/pb_encode.c $(AA_DIR)/nanopb/pb_decode.c \
	  $(shell find $(AA_DIR)/proto -name '*.pb.c') -o $(OUT_DIR)/aa_mt_test $(LDFLAGS)

# Socketpair round-trip for the wireless (aaw) Bluetooth handshake.
aa_aaw_test: aa_aaw_test.cpp
	$(CXX) $(CXXFLAGS) -DUSE_AA_WIRELESS -I../src aa_aaw_test.cpp \
	  $(AA_DIR)/aa_aaw.cpp ../src/common/logger.cpp -o $(OUT_DIR)/aa_aaw_test $(LDFLAGS) -pthread

# Smoke-test the Bluetooth D-Bus service against a live BlueZ (run as root).
aa_bt_test: aa_bt_test.cpp
	$(CXX) $(CXXFLAGS) -DUSE_AA_WIRELESS -I../src $(shell pkg-config --cflags dbus-1) aa_bt_test.cpp \
	  $(AA_DIR)/aa_bluetooth.cpp $(AA_DIR)/aa_aaw.cpp ../src/settings.cpp ../src/common/logger.cpp \
	  -o $(OUT_DIR)/aa_bt_test $(LDFLAGS) $(shell pkg-config --libs dbus-1) -lbluetooth -pthread

# Round-trip for the user-settings override layer (UI persistence).
settings_user_test: settings_user_test.cpp
	$(CXX) $(CXXFLAGS) -I../src settings_user_test.cpp \
	  ../src/settings.cpp ../src/common/logger.cpp \
	  -o $(OUT_DIR)/settings_user_test $(LDFLAGS)

# MFi authentication coprocessor bring-up probe (CarPlay P0).
mfi_probe: mfi_probe.cpp
	$(CXX) $(CXXFLAGS) -I../src mfi_probe.cpp \
	  ../src/protocol/cp/mfi_auth.cpp -o $(OUT_DIR)/mfi_probe $(LDFLAGS)

keylistener: keylistener.cpp
	$(CXX) $(CXXFLAGS) keylistener.cpp -o $(OUT_DIR)/keylistener $(LDFLAGS)

emulator: emulator.cpp
	$(CXX) $(CXXFLAGS) emulator.cpp -o $(OUT_DIR)/emulator $(LDFLAGS) -llgpio

clean:
	rm -f $(OUT_DIR)/keylistener $(OUT_DIR)/emulator