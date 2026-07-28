// CarPlay wireless control-session message tests: build the CarPlayStartSession
// Wi-Fi handoff (0x4301) and CarPlayAvailability (0x4300), parse them back, and
// verify the nested wireless attributes round-trip. Pure codec, no BT/phone.
//
//   make cp_carplay_msg_test && ../out/cp_carplay_msg_test

#include <cstdio>

#include "protocol/cp/cp_carplay_msg.h"

using cp_carplay::Bytes;

static int failures = 0;
static void check(bool ok, const char *what)
{
    printf("  %-56s %s\n", what, ok ? "ok" : "FAILED");
    if (!ok) failures++;
}

int main()
{
    printf("CarPlay wireless CSM tests\n\n");

    printf("CarPlayStartSession (0x4301) Wi-Fi handoff:\n");
    {
        cp_carplay::WirelessSession s;
        s.ssid = "FastCarPlay-AP";
        s.passphrase = "drive1234";
        s.channel = 6;
        s.ipAddress = "192.168.4.1";
        s.security = cp_carplay::WifiSecurity::WpaWpa2;
        s.port = 7000;
        s.deviceIdentifier = "3E7C1A9B-0000-1000-8000-00805F9B34FB";
        s.publicKey = "a1b2c3d4e5f6";
        s.sourceVersion = "550.1";

        Bytes msg = cp_carplay::buildStartSession(s);
        check(msg[0] == 0x40 && msg[1] == 0x40, "CSM starts 0x4040");
        // msgId is at offset 4 (after 0x4040 + u16 length).
        check(msg[4] == 0x43 && msg[5] == 0x01, "message id is 0x4301");

        cp_carplay::WirelessSession out;
        check(cp_carplay::parseStartSession(msg, out), "StartSession parses");
        check(out.ssid == s.ssid && out.passphrase == s.passphrase, "ssid + passphrase round-trip");
        check(out.channel == 6 && out.ipAddress == s.ipAddress, "channel + ip round-trip");
        check(out.security == cp_carplay::WifiSecurity::WpaWpa2, "security type round-trips");
        check(out.port == 7000, "control port round-trips");
        check(out.deviceIdentifier == s.deviceIdentifier && out.publicKey == s.publicKey,
              "pairing identity (pi/pk) round-trips");
        check(out.sourceVersion == "550.1", "source version round-trips");
    }

    printf("\nCarPlayAvailability (0x4300):\n");
    {
        Bytes msg = cp_carplay::buildCarPlayAvailability("00:11:22:33:44:55");
        check(msg[4] == 0x43 && msg[5] == 0x00, "message id is 0x4300");

        uint16_t msgId = 0;
        std::vector<cp_carplay::CsmParam> params;
        check(cp_iap2::parseCsm(msg, msgId, params), "availability CSM parses");
        // wireless_attributes is param 1; unpack its nested group.
        bool foundBt = false;
        for (const auto &p : params)
        {
            if (p.id != 1) continue;
            std::vector<cp_carplay::CsmParam> w;
            if (cp_carplay::parseGroup(p.value, w))
                for (const auto &wp : w)
                    if (wp.id == 1 && wp.value.size() >= 17)
                        foundBt = true;
        }
        check(foundBt, "wireless attributes carry the BT transport id");
    }

    printf("\nIdentificationInformation (0x1D01) wireless CarPlay identity:\n");
    {
        cp_carplay::AccessoryIdentity id;
        id.bluetoothMac = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};
        id.messagesSent = cp_carplay::defaultMessagesSent();
        id.messagesReceived = cp_carplay::defaultMessagesReceived();

        Bytes msg = cp_carplay::buildIdentification(id);
        check(msg[4] == 0x1D && msg[5] == 0x01, "message id is 0x1D01");

        uint16_t msgId = 0;
        std::vector<cp_carplay::CsmParam> params;
        check(cp_iap2::parseCsm(msg, msgId, params), "identification parses");

        bool hasWirelessCarPlay = false, hasBtMac = false, hasName = false;
        for (const auto &p : params)
        {
            if (p.id == 0 && p.value.size() >= 12 /* "FastCarPlay\0" */) hasName = true;
            if (p.id == 24) // wireless_car_play_transport_component
            {
                std::vector<cp_carplay::CsmParam> g;
                if (cp_carplay::parseGroup(p.value, g))
                    for (const auto &gp : g)
                        if (gp.id == 4) hasWirelessCarPlay = true; // supports_car_play flag
            }
            if (p.id == 17) // bluetooth_transport_component
            {
                std::vector<cp_carplay::CsmParam> g;
                if (cp_carplay::parseGroup(p.value, g))
                    for (const auto &gp : g)
                        if (gp.id == 3 && gp.value == Bytes{0xAA,0xBB,0xCC,0xDD,0xEE,0xFF})
                            hasBtMac = true;
            }
        }
        check(hasName, "carries the accessory name");
        check(hasWirelessCarPlay, "declares the wireless CarPlay transport (supports_car_play)");
        check(hasBtMac, "bluetooth transport component carries the 6-byte MAC");
    }

    printf("\nnested group encode/parse:\n");
    {
        Bytes g = cp_carplay::encGroup({{0, cp_carplay::encStr("hi")}, {7, cp_carplay::encU32(42)}});
        std::vector<cp_carplay::CsmParam> out;
        check(cp_carplay::parseGroup(g, out) && out.size() == 2 && out[0].id == 0 && out[1].id == 7,
              "group round-trips two params");
    }

    printf("\n%s\n", failures == 0 ? "PASS" : "FAIL");
    return failures == 0 ? 0 : 1;
}
