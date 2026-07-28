// iAP2 codec round-trip tests: link packets (with/without payload), the SYN
// link-sync payload, and control-session messages. Pure framing, no USB/phone.
//
//   make cp_iap2_test && ../out/cp_iap2_test

#include <cstdio>

#include "protocol/cp/cp_iap2.h"

using cp_iap2::Bytes;

static int failures = 0;
static void check(bool ok, const char *what)
{
    printf("  %-56s %s\n", what, ok ? "ok" : "FAILED");
    if (!ok) failures++;
}

int main()
{
    printf("iAP2 codec tests\n\n");

    printf("link packet framing:\n");
    {
        // ACK: header only, length 9, valid checksum.
        Bytes ack = cp_iap2::packPacket(cp_iap2::CONTROL_ACK, 5, 3, 0, {});
        check(ack.size() == 9 && ack[0] == 0xFF && ack[1] == 0x5A, "ACK is a 9-byte 0xFF5A packet");
        cp_iap2::LinkHeader h;
        Bytes payload;
        check(cp_iap2::parsePacket(ack, h, payload) && h.control == cp_iap2::CONTROL_ACK &&
                  h.seq == 5 && h.ack == 3 && payload.empty(),
              "ACK round-trips");

        // Data packet with payload.
        Bytes data = {0xDE, 0xAD, 0xBE, 0xEF, 0x01, 0x02};
        Bytes pkt = cp_iap2::packPacket(0, 7, 4, 0x0A, data);
        check(pkt.size() == data.size() + 10 && h.length <= pkt.size(), "data packet size = payload+10");
        check(cp_iap2::parsePacket(pkt, h, payload) && payload == data && h.sessionId == 0x0A,
              "data packet round-trips (both checksums valid)");

        // Corruption is rejected.
        pkt[10] ^= 1;
        check(!cp_iap2::parsePacket(pkt, h, payload), "corrupted payload is rejected");
    }

    printf("\nlink synchronization (SYN) payload:\n");
    {
        cp_iap2::LinkSync sync;
        sync.maxOutgoing = 4;
        sync.maxLen = 1500;
        sync.retransmissionTimeout = 1600;
        sync.ackTimeout = 1000;
        sync.maxRetransmissions = 3;
        sync.maxAck = 3;
        sync.sessions = {{0x0A, 0x00, 0x01}, {0x0B, 0x01, 0x01}}; // control + EA sessions

        Bytes packed = cp_iap2::packSync(sync);
        cp_iap2::LinkSync out;
        check(cp_iap2::parseSync(packed, out), "SYN parses");
        check(out.maxLen == 1500 && out.maxOutgoing == 4 && out.sessions.size() == 2 &&
                  out.sessions[1].id == 0x0B && out.sessions[1].type == 0x01,
              "SYN round-trips with sessions");

        // The SYN travels as a link packet with the SYN control bit.
        Bytes synPkt = cp_iap2::packPacket(cp_iap2::CONTROL_SYN, 0, 0, 0, packed);
        cp_iap2::LinkHeader h;
        Bytes body;
        check(cp_iap2::parsePacket(synPkt, h, body) && (h.control & cp_iap2::CONTROL_SYN) && body == packed,
              "SYN packet carries the sync payload");
    }

    printf("\ncontrol-session message (CSM):\n");
    {
        // A message with two parameters.
        std::vector<cp_iap2::CsmParam> params = {
            {0x0000, {'F', 'a', 's', 't', 'C', 'a', 'r', 'P', 'l', 'a', 'y'}}, // e.g. Name
            {0x0001, {0x01}},                                                  // e.g. a flag
        };
        Bytes msg = cp_iap2::packCsm(0x1D00, params); // e.g. IdentificationInformation
        check(msg[0] == 0x40 && msg[1] == 0x40, "CSM starts 0x4040");

        uint16_t msgId = 0;
        std::vector<cp_iap2::CsmParam> out;
        check(cp_iap2::parseCsm(msg, msgId, out), "CSM parses");
        check(msgId == 0x1D00 && out.size() == 2 && out[0].id == 0x0000 &&
                  out[0].value.size() == 11 && out[1].value == Bytes{0x01},
              "CSM round-trips with parameters");
    }

    printf("\n%s\n", failures == 0 ? "PASS" : "FAIL");
    return failures == 0 ? 0 : 1;
}
