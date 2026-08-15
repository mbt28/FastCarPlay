#include "status_file.h"

#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "autogen/version.h"
#include "common/logger.h"

namespace
{
// A reader polls this; anything longer and a status page feels dead, anything
// shorter is just noise. Only a heartbeat -- real changes are written at once.
constexpr int64_t HEARTBEAT_MS = 5000;

int64_t nowMs()
{
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

void appendEscaped(std::string &out, const std::string &in)
{
    out += '"';
    for (const char c : in)
    {
        switch (c)
        {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            // Control characters would make the document unparseable; a status
            // string comes from a backend and is not guaranteed clean.
            if (static_cast<unsigned char>(c) < 0x20)
            {
                static const char *hex = "0123456789abcdef";
                out += "\\u00";
                out += hex[(c >> 4) & 0xF];
                out += hex[c & 0xF];
            }
            else
            {
                out += c;
            }
        }
    }
    out += '"';
}
} // namespace

StatusFile::StatusFile(std::string path) : _path(std::move(path))
{
    // Overridable: on the head unit this is /run (root-owned, which the app is),
    // but a desktop build runs as a normal user and the tests need somewhere
    // writable.
    if (const char *env = getenv("FCP_STATUS_FILE"); env != nullptr && *env != '\0')
        _path = env;

    const std::size_t slash = _path.find_last_of('/');
    _dir = (slash == std::string::npos) ? std::string(".") : _path.substr(0, slash);
    if (mkdir(_dir.c_str(), 0755) != 0 && errno != EEXIST)
    {
        log_w("status: cannot create %s > %s -- status file disabled",
              _dir.c_str(), strerror(errno));
        _disabled = true;
    }
}

StatusFile::~StatusFile()
{
    // Remove it on a clean exit so a reader sees "not running" immediately
    // rather than waiting for the mtime to go stale. A crash leaves the file,
    // which the staleness check covers.
    if (!_disabled)
        unlink(_path.c_str());
}

bool StatusFile::due(int state, bool videoFocused) const
{
    if (_disabled)
        return false;
    if (!_written || state != _last.state || videoFocused != _last.videoFocused)
        return true;
    return nowMs() - _lastWriteMs >= HEARTBEAT_MS;
}

void StatusFile::update(const Fields &fields)
{
    if (_disabled)
        return;

    const bool changed = !_written ||
                         fields.state != _last.state ||
                         fields.videoFocused != _last.videoFocused ||
                         fields.status != _last.status ||
                         fields.phoneName != _last.phoneName ||
                         fields.protocol != _last.protocol;

    const int64_t now = nowMs();
    if (!changed && now - _lastWriteMs < HEARTBEAT_MS)
        return;

    if (!writeNow(fields))
        return;

    _last = fields;
    _written = true;
    _lastWriteMs = now;
}

bool StatusFile::writeNow(const Fields &fields)
{
    std::string body = "{\n";
    body += "  \"pid\": " + std::to_string(getpid()) + ",\n";
    body += "  \"app_version\": ";
    appendEscaped(body, FCP_VERSION);
    body += ",\n  \"app_build\": " + std::to_string(FCP_BUILD) + ",\n";
    body += "  \"state\": " + std::to_string(fields.state) + ",\n";
    body += "  \"status\": ";
    appendEscaped(body, fields.status);
    body += ",\n  \"phone_name\": ";
    appendEscaped(body, fields.phoneName);
    body += ",\n  \"protocol\": ";
    appendEscaped(body, fields.protocol);
    body += ",\n  \"video_focused\": ";
    body += fields.videoFocused ? "true" : "false";
    body += ",\n  \"bytes\": " + std::to_string(fields.transfered) + "\n}\n";

    // Written via tmp+rename so a reader never sees a half-written document.
    // No fsync: this is a tmpfs, and durability across a power cut is exactly
    // what a liveness file must NOT have.
    const std::string tmp = _path + ".tmp";
    const int fd = open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
    {
        log_w("status: cannot write %s > %s -- status file disabled",
              tmp.c_str(), strerror(errno));
        _disabled = true;
        return false;
    }

    const char *p = body.data();
    std::size_t left = body.size();
    while (left > 0)
    {
        const ssize_t n = write(fd, p, left);
        if (n <= 0)
        {
            if (n < 0 && errno == EINTR)
                continue;
            close(fd);
            unlink(tmp.c_str());
            return false;
        }
        p += n;
        left -= static_cast<std::size_t>(n);
    }
    close(fd);

    if (rename(tmp.c_str(), _path.c_str()) != 0)
    {
        unlink(tmp.c_str());
        return false;
    }
    return true;
}
