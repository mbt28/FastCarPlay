#ifndef SRC_COMMON_STATUS_FILE
#define SRC_COMMON_STATUS_FILE

#include <cstdint>
#include <string>

// Publishes what the app is doing to a small JSON file so a separate process --
// the on-device settings/update service -- can show live status without any
// protocol between us.
//
// It lives on /run deliberately. /run is a tmpfs, and this file is rewritten
// for as long as the app runs; /tmp on the head unit is backed by the SD card,
// where a heartbeat would be a pointless stream of flash writes.
//
// A reader treats a stale mtime or a dead pid as "the app is not running". That
// is the whole liveness mechanism -- no signals, no socket, and it reports the
// truth in the one case the service exists for: the app has crashed or hung.
class StatusFile
{
public:
    struct Fields
    {
        int state = 0;
        std::string status;
        std::string phoneName;
        std::string protocol;
        bool videoFocused = false;
        uint32_t transfered = 0;
    };

    explicit StatusFile(std::string path = "/run/fastcarplay/status.json");
    ~StatusFile();

    // Cheap pre-check, so a caller can skip building the Fields at all on the
    // frames where nothing would be written. status() allocates a string on
    // every call in some backends, and the render loop runs at 60 Hz.
    bool due(int state, bool videoFocused) const;

    // Writes only when a reader-visible field changed, or the heartbeat fell
    // due. Call it behind due().
    void update(const Fields &fields);

private:
    bool writeNow(const Fields &fields);

    std::string _path;
    std::string _dir;
    Fields _last;
    bool _written = false;
    bool _disabled = false; // a path we cannot write is not worth retrying
    int64_t _lastWriteMs = 0;
};

#endif /* SRC_COMMON_STATUS_FILE */
