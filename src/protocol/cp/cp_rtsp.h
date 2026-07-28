#ifndef SRC_PROTOCOL_CP_CP_RTSP
#define SRC_PROTOCOL_CP_CP_RTSP

// RTSP/HTTP-style request framing for the CarPlay control channel: a text
// request line + headers + optional binary body (Content-Length). Parses
// incoming requests incrementally and builds responses. Encryption, once the
// handshake completes, wraps this framing in a separate layer. Mirrors LIVI
// rtspMessage.ts.

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace cp_rtsp
{
using Bytes = std::vector<uint8_t>;

struct Request
{
    std::string method;
    std::string path;
    std::string protocol;
    std::map<std::string, std::string> headers; // names lower-cased
    Bytes body;

    std::string header(const std::string &name) const
    {
        auto it = headers.find(name);
        return it == headers.end() ? std::string() : it->second;
    }
};

struct Response
{
    int status = 200;
    std::map<std::string, std::string> headers;
    Bytes body;
};

// Parse as many complete messages as `buf` holds; consumed bytes are removed
// from `buf` (a partial trailing message is left for the next call).
std::vector<Request> parse(Bytes &buf);

// Build a response, echoing the request protocol + its CSeq, setting
// Content-Length.
Bytes build(const Request &req, const Response &res);
} // namespace cp_rtsp

#endif /* SRC_PROTOCOL_CP_CP_RTSP */
