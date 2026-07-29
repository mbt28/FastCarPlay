#ifndef SRC_PROTOCOL_CP_CP_AV
#define SRC_PROTOCOL_CP_CP_AV

// CarPlay AV layer: the media protocol that runs over the encrypted control
// channel after pairing. The phone drives it with RTSP-style requests carrying
// binary plists -- GET /info (capabilities), SETUP (session then per-stream),
// RECORD, POST /command|/feedback -- and then opens TCP connections to the
// ports we hand back to carry the H.264 screen video and audio. This module
// owns those listeners and the per-stream crypto (all keys derived from the
// pair-verify shared secret). Mirrors LIVI cp/stack/cpStack.ts.
//
// Milestone A: complete the negotiation (so the phone starts streaming) and the
// event channel, and receive the stream data. Decode/display/audio/touch land
// in later milestones; received stream bytes can be captured for analysis.

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <netinet/in.h>

#include "cp_rtsp.h"

namespace cp_control_cipher { class ControlCipher; }

namespace cp_av
{
using Bytes = std::vector<uint8_t>;

// CarPlay stream types (LIVI cpStack.ts).
constexpr int STREAM_MAIN_SCREEN = 110;
constexpr int STREAM_ALT_SCREEN = 111;
constexpr int STREAM_MAIN_AUDIO = 100;
constexpr int STREAM_ALT_AUDIO = 101;
constexpr int STREAM_MAIN_HIGH_AUDIO = 102;
constexpr int STREAM_DATA = 130;

struct Config
{
    int screenWidth = 800;
    int screenHeight = 480;
    int screenWidthMm = 154;
    int screenHeightMm = 90;
    int fps = 30;
    bool hevc = true; // iOS 26 negotiates HEVC for wireless CarPlay
};

// Callbacks for received media (set before the streams connect). Milestone A
// leaves these optional; later milestones feed the decoder/audio.
struct Sinks
{
    // A complete video access unit (H.264 Annex-B, phone -> us).
    std::function<void(const Bytes &)> onVideo;
    // A decoded/opaque audio payload with its CarPlay stream type.
    std::function<void(int type, const Bytes &)> onAudio;
};

class AvSession
{
public:
    AvSession(const Bytes &pairVerifyShared, const Config &cfg);
    ~AvSession();

    void setSinks(const Sinks &sinks) { _sinks = sinks; }

    // The controller's address (from the control connection). Needed so the
    // UDP timing sync can reach the phone's timing port on the same link.
    void setPeer(const struct sockaddr_in6 &peer) { _peer = peer; _havePeer = true; }

    // Handle one AV RTSP request. Returns true and fills `res` if it's an AV
    // method; returns false if the caller should treat it as unknown.
    bool handle(const cp_rtsp::Request &req, cp_rtsp::Response &res);

    void stop();

private:
    struct Listener
    {
        int fd = -1;
        uint16_t port = 0;
        std::thread thread;
    };

    // Bind an ephemeral dual-stack TCP port; returns the port (0 on failure).
    uint16_t openListener(Listener &l, const char *tag,
                          std::function<void(int)> onClient);

    cp_rtsp::Response handleInfo(const cp_rtsp::Request &req);
    cp_rtsp::Response handleSetup(const cp_rtsp::Request &req);
    cp_rtsp::Response handleFeedback(const cp_rtsp::Request &req);

    // UDP timing (RTCP-style NTP): bind a port, answer the phone's requests, and
    // drive periodic requests to the phone's timing port. Returns our port.
    uint16_t startTiming(uint16_t phoneTimingPort);
    void timingLoop(uint16_t phoneTimingPort);

    void eventLoop(int fd);
    void screenLoop(int fd, int64_t streamId);
    void audioLoop(int fd, int64_t streamId, int type);

    // Derive a stream data key: HKDF-SHA512(shared, "DataStream-Salt"<id>, info).
    Bytes streamKey(int64_t streamId, const char *info);

    Config _cfg;
    Bytes _shared;
    Sinks _sinks;
    std::atomic<bool> _running{true};

    Listener _event;
    Listener _keepAlive;
    std::vector<std::unique_ptr<Listener>> _streams;

    struct sockaddr_in6 _peer{};
    bool _havePeer = false;
    int _timingFd = -1;
    std::thread _timingThread;

    std::unique_ptr<cp_control_cipher::ControlCipher> _eventCipher;
};
} // namespace cp_av

#endif /* SRC_PROTOCOL_CP_CP_AV */
