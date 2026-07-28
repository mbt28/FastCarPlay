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

# CarPlay crypto substrate: known-answer + round-trip tests (no chip/phone).
cp_crypto_test: cp_crypto_test.cpp
	$(CXX) $(CXXFLAGS) -I../src cp_crypto_test.cpp \
	  ../src/protocol/cp/cp_crypto.cpp -o $(OUT_DIR)/cp_crypto_test $(LDFLAGS) $(shell pkg-config --libs openssl)

# CarPlay MFiSAP /auth-setup end-to-end test against the real MFi chip.
CP_DIR := ../src/protocol/cp
cp_authsetup_test: cp_authsetup_test.cpp
	$(CXX) $(CXXFLAGS) -I../src cp_authsetup_test.cpp \
	  $(CP_DIR)/cp_auth_setup.cpp $(CP_DIR)/cp_crypto.cpp $(CP_DIR)/mfi_auth.cpp \
	  -o $(OUT_DIR)/cp_authsetup_test $(LDFLAGS) $(shell pkg-config --libs openssl)

# CarPlay pair-verify end-to-end test (no chip/phone; symmetric crypto).
cp_pairverify_test: cp_pairverify_test.cpp
	$(CXX) $(CXXFLAGS) -I../src cp_pairverify_test.cpp \
	  ../src/protocol/cp/cp_pair_verify.cpp ../src/protocol/cp/cp_identity.cpp \
	  ../src/protocol/cp/cp_tlv8.cpp ../src/protocol/cp/cp_crypto.cpp \
	  ../src/common/logger.cpp \
	  -o $(OUT_DIR)/cp_pairverify_test $(LDFLAGS) $(shell pkg-config --libs openssl)

# CarPlay pair-setup end-to-end test (SRP-6a client; no chip/phone).
cp_pairsetup_test: cp_pairsetup_test.cpp
	$(CXX) $(CXXFLAGS) -I../src cp_pairsetup_test.cpp \
	  ../src/protocol/cp/cp_pair_setup.cpp ../src/protocol/cp/cp_srp.cpp \
	  ../src/protocol/cp/cp_pair_verify.cpp ../src/protocol/cp/cp_identity.cpp \
	  ../src/protocol/cp/cp_tlv8.cpp ../src/protocol/cp/cp_crypto.cpp \
	  ../src/common/logger.cpp \
	  -o $(OUT_DIR)/cp_pairsetup_test $(LDFLAGS) $(shell pkg-config --libs openssl)

# CarPlay control-channel integration test (RTSP + encryption + MFiSAP).
cp_control_test: cp_control_test.cpp
	$(CXX) $(CXXFLAGS) -I../src cp_control_test.cpp \
	  ../src/protocol/cp/cp_control_channel.cpp ../src/protocol/cp/cp_control_cipher.cpp \
	  ../src/protocol/cp/cp_rtsp.cpp ../src/protocol/cp/cp_auth_setup.cpp \
	  ../src/protocol/cp/cp_pair_setup.cpp ../src/protocol/cp/cp_srp.cpp \
	  ../src/protocol/cp/cp_pair_verify.cpp ../src/protocol/cp/cp_identity.cpp \
	  ../src/protocol/cp/cp_tlv8.cpp ../src/protocol/cp/cp_crypto.cpp \
	  ../src/protocol/cp/mfi_auth.cpp ../src/common/logger.cpp \
	  -o $(OUT_DIR)/cp_control_test $(LDFLAGS) $(shell pkg-config --libs openssl)

# CarPlay mDNS responder test (announces _airplay._tcp; self-verifying).
cp_mdns_test: cp_mdns_test.cpp
	$(CXX) $(CXXFLAGS) -I../src cp_mdns_test.cpp \
	  ../src/protocol/cp/cp_mdns.cpp ../src/protocol/cp/cp_identity.cpp \
	  ../src/protocol/cp/cp_crypto.cpp ../src/common/logger.cpp \
	  -o $(OUT_DIR)/cp_mdns_test $(LDFLAGS) $(shell pkg-config --libs openssl) -pthread

# CarPlay TCP server integration test (full handshake over a real socket).
cp_server_test: cp_server_test.cpp
	$(CXX) $(CXXFLAGS) -I../src cp_server_test.cpp \
	  ../src/protocol/cp/cp_server.cpp ../src/protocol/cp/cp_control_channel.cpp \
	  ../src/protocol/cp/cp_control_cipher.cpp ../src/protocol/cp/cp_rtsp.cpp \
	  ../src/protocol/cp/cp_auth_setup.cpp ../src/protocol/cp/cp_pair_setup.cpp \
	  ../src/protocol/cp/cp_srp.cpp ../src/protocol/cp/cp_pair_verify.cpp \
	  ../src/protocol/cp/cp_identity.cpp ../src/protocol/cp/cp_tlv8.cpp \
	  ../src/protocol/cp/cp_crypto.cpp ../src/protocol/cp/mfi_auth.cpp \
	  ../src/common/logger.cpp \
	  -o $(OUT_DIR)/cp_server_test $(LDFLAGS) $(shell pkg-config --libs openssl) -pthread

# cp_serve -- the runnable CarPlay accessory daemon (identity+mDNS+TCP+chip).
cp_serve: cp_serve.cpp
	$(CXX) $(CXXFLAGS) -I../src cp_serve.cpp \
	  ../src/protocol/cp/cp_mdns.cpp ../src/protocol/cp/cp_server.cpp \
	  ../src/protocol/cp/cp_control_channel.cpp ../src/protocol/cp/cp_control_cipher.cpp \
	  ../src/protocol/cp/cp_rtsp.cpp ../src/protocol/cp/cp_auth_setup.cpp \
	  ../src/protocol/cp/cp_pair_setup.cpp ../src/protocol/cp/cp_srp.cpp \
	  ../src/protocol/cp/cp_pair_verify.cpp ../src/protocol/cp/cp_identity.cpp \
	  ../src/protocol/cp/cp_tlv8.cpp ../src/protocol/cp/cp_crypto.cpp \
	  ../src/protocol/cp/mfi_auth.cpp ../src/common/logger.cpp \
	  -o $(OUT_DIR)/cp_serve $(LDFLAGS) $(shell pkg-config --libs openssl) -pthread

# iAP2 codec round-trip tests (link packets, SYN, control-session messages).
cp_iap2_test: cp_iap2_test.cpp
	$(CXX) $(CXXFLAGS) -I../src cp_iap2_test.cpp \
	  ../src/protocol/cp/cp_iap2.cpp -o $(OUT_DIR)/cp_iap2_test $(LDFLAGS)

# CarPlay wireless control-session messages: the Wi-Fi handoff (StartSession).
cp_carplay_msg_test: cp_carplay_msg_test.cpp
	$(CXX) $(CXXFLAGS) -I../src cp_carplay_msg_test.cpp \
	  $(CP_DIR)/cp_carplay_msg.cpp $(CP_DIR)/cp_iap2.cpp \
	  -o $(OUT_DIR)/cp_carplay_msg_test $(LDFLAGS)

# CarPlay USB probe: map a plugged iPhone's iAP2 interface + endpoints.
cp_usb_probe: cp_usb_probe.cpp
	$(CXX) $(CXXFLAGS) -I../src cp_usb_probe.cpp -o $(OUT_DIR)/cp_usb_probe $(LDFLAGS) $(shell pkg-config --libs libusb-1.0)

# CarPlay wired trigger: reveal the iPhone's hidden CarPlay USB configs.
cp_usb_trigger: cp_usb_trigger.cpp
	$(CXX) $(CXXFLAGS) -I../src cp_usb_trigger.cpp -o $(OUT_DIR)/cp_usb_trigger $(LDFLAGS) $(shell pkg-config --libs libusb-1.0)

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