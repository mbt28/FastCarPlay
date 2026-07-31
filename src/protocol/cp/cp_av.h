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

#include "cp_audio.h"
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

// A source of outbound event-channel commands (touch/button HID reports, etc.)
// the accessory sends to the phone. The AV session pulls from it on the event
// channel's own thread and POSTs each body, so the event cipher stays
// single-threaded. The consumer (a connection backend) builds the binary-plist
// bodies (e.g. {type:'hidSendReport', uuid, hidReport}).
struct InputSource
{
    virtual ~InputSource() = default;
    // Pop the next pending command body (a binary plist) to POST to the phone,
    // or return false if none is queued. Called on the event-channel thread.
    virtual bool nextCommand(std::vector<uint8_t> &body) = 0;
};

// Callbacks for received media (set before the streams connect). Optional --
// unset sinks are simply not called (the daemon runs headless; the F1C app wires
// these to the decoder/display).
struct Sinks
{
    // The negotiated screen codec, reported once before the first access unit
    // (the phone may pick H.264 even when H.265 is offered). true = HEVC/H.265.
    std::function<void(bool hevc)> onVideoCodec;
    // A screen video access unit as Annex-B (00 00 00 01 start codes): first the
    // config parameter sets, then decoded frames. Ready for avcodec/cedrus.
    std::function<void(const Bytes &)> onVideo;
    // Decoded audio: the CarPlay stream type (100/101 nav-speech, 102 media), the
    // PCM rate + channel count, and S16 interleaved (native-endian) samples.
    std::function<void(int type, int rate, int channels, const Bytes &pcm)> onAudio;
    // The phone tapped the CarPlay dock's car/home icon (a POST /command
    // {type:'requestUI'}), asking us to bring the head-unit's own UI to the
    // foreground -- the CarPlay equivalent of Android Auto's host-ui-requested.
    // Optional; wired to the connection's video-focus release.
    std::function<void()> onRequestNativeUI;
};

class AvSession
{
public:
    AvSession(const Bytes &pairVerifyShared, const Config &cfg);
    ~AvSession();

    void setSinks(const Sinks &sinks) { _sinks = sinks; }

    // Outbound input (touch/buttons) to forward to the phone over the event
    // channel. Optional; the pointer must outlive the session.
    void setInputSource(InputSource *src) { _inputSource = src; }

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
        int fd = -1;     // TCP listen socket, or UDP data socket (audio)
        int ctrlFd = -1; // UDP RTCP control socket (audio only)
        uint16_t port = 0;
        std::thread thread;
    };

    // Bind an ephemeral dual-stack TCP port; returns the port (0 on failure).
    uint16_t openListener(Listener &l, const char *tag,
                          std::function<void(int)> onClient);
    // Bind an ephemeral dual-stack UDP socket into `fd`; returns the port.
    uint16_t openUdp(int &fd);

    cp_rtsp::Response handleInfo(const cp_rtsp::Request &req);
    cp_rtsp::Response handleSetup(const cp_rtsp::Request &req);
    cp_rtsp::Response handleFeedback(const cp_rtsp::Request &req);

    // UDP timing (RTCP-style NTP): bind a port, answer the phone's requests, and
    // drive periodic requests to the phone's timing port. Returns our port.
    uint16_t startTiming(uint16_t phoneTimingPort);
    void timingLoop(uint16_t phoneTimingPort);

    void eventLoop(int fd);
    // Send one event-channel command (a binary plist body) to the phone as a
    // reverse-HTTP POST /command. Called only from the event-channel thread.
    void sendEventCommand(int fd, const Bytes &body);
    void screenLoop(int fd, int64_t streamId);
    // Audio is UDP/RTP: receive on the data socket, drain RTCP on the control
    // socket, decrypt each packet, decode the access unit, and emit PCM.
    void audioLoop(int dataFd, int ctrlFd, int64_t streamId, int type, cp_audio::Codec codec,
                   int rate, int channels, bool upmix);
    // The iAP2-over-CarPlay tunnel (stream 130): the phone continues iAP2 here
    // after it drops Bluetooth. Receive-only (our replies ride the event channel).
    void tunnelLoop(int fd, int64_t seed);

    // Derive a stream data key: HKDF-SHA512(shared, "DataStream-Salt"<id>, info).
    Bytes streamKey(int64_t streamId, const char *info);

    Config _cfg;
    Bytes _shared;
    Sinks _sinks;
    InputSource *_inputSource = nullptr;
    unsigned _eventCseq = 0; // reverse-HTTP CSeq for our event commands
    std::atomic<bool> _running{true};

    Listener _event;
    std::vector<std::unique_ptr<Listener>> _streams;

    struct sockaddr_in6 _peer{};
    bool _havePeer = false;
    int _timingFd = -1;
    std::thread _timingThread;
    int _keepAliveFd = -1; // UDP, like the phone expects (not TCP)
    std::thread _keepAliveThread;
    int64_t _clockOffsetNtp = 0; // steers our clock onto the phone's NTP domain

    std::unique_ptr<cp_control_cipher::ControlCipher> _eventCipher;
};
} // namespace cp_av

#endif /* SRC_PROTOCOL_CP_CP_AV */
