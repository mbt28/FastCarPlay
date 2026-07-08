#ifndef SRC_PROTOCOL_AA_AA_AAW
#define SRC_PROTOCOL_AA_AA_AAW

#ifdef USE_AA_WIRELESS

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

// Android Auto Wireless (aaw) credential handshake, spoken over the Bluetooth
// RFCOMM channel. Frame = [len u16BE][msgId u16BE][protobuf]. The head unit
// tells the phone its TCP endpoint (WifiStartRequest) and the AP credentials
// (WifiInfoResponse); the phone then joins the AP and connects to the TCP
// server. Message bodies are built by hand to match the on-wire format.
namespace aa_aaw
{
struct Params
{
    std::string ip;      // head unit AP IP
    uint16_t port;       // TCP server port (5277)
    std::string ssid;
    std::string passphrase;
    std::string bssid;
    int channel;         // Wi-Fi channel (for the version request frequency)
};

// Frame builders (each returns a full [len][msgId][proto] frame).
std::vector<uint8_t> versionRequest(int channel);
std::vector<uint8_t> startRequest(const std::string &ip, uint16_t port);
std::vector<uint8_t> infoResponse(const std::string &ssid, const std::string &pass,
                                  const std::string &bssid);

// Run the full handshake on a connected RFCOMM socket, then keep it alive
// (answering pings) until it drops or `active` goes false. Returns true if the
// credential exchange completed.
bool runHandshake(int fd, const Params &params, std::atomic<bool> &active);
} // namespace aa_aaw

#endif /* USE_AA_WIRELESS */
#endif /* SRC_PROTOCOL_AA_AA_AAW */
