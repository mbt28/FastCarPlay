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

keylistener: keylistener.cpp
	$(CXX) $(CXXFLAGS) keylistener.cpp -o $(OUT_DIR)/keylistener $(LDFLAGS)

emulator: emulator.cpp
	$(CXX) $(CXXFLAGS) emulator.cpp -o $(OUT_DIR)/emulator $(LDFLAGS) -llgpio

clean:
	rm -f $(OUT_DIR)/keylistener $(OUT_DIR)/emulator