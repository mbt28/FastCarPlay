#include "cp_rtsp.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <sstream>

namespace cp_rtsp
{
namespace
{
std::string lower(std::string s)
{
    for (char &c : s)
        c = (char)std::tolower((unsigned char)c);
    return s;
}
std::string trim(const std::string &s)
{
    size_t a = 0, b = s.size();
    while (a < b && std::isspace((unsigned char)s[a])) a++;
    while (b > a && std::isspace((unsigned char)s[b - 1])) b--;
    return s.substr(a, b - a);
}

// Find "\r\n\r\n" from `from`; returns npos if absent.
size_t findHeaderEnd(const Bytes &buf, size_t from)
{
    static const uint8_t sep[4] = {'\r', '\n', '\r', '\n'};
    if (buf.size() < 4)
        return std::string::npos;
    for (size_t i = from; i + 4 <= buf.size(); i++)
        if (memcmp(buf.data() + i, sep, 4) == 0)
            return i;
    return std::string::npos;
}
} // namespace

std::vector<Request> parse(Bytes &buf)
{
    std::vector<Request> out;
    size_t offset = 0;

    while (offset < buf.size())
    {
        const size_t headerEnd = findHeaderEnd(buf, offset);
        if (headerEnd == std::string::npos)
            break;

        std::string headerText((const char *)buf.data() + offset, headerEnd - offset);
        // Split into lines on \r\n.
        std::vector<std::string> lines;
        size_t p = 0;
        while (p <= headerText.size())
        {
            size_t nl = headerText.find("\r\n", p);
            if (nl == std::string::npos)
            {
                lines.push_back(headerText.substr(p));
                break;
            }
            lines.push_back(headerText.substr(p, nl - p));
            p = nl + 2;
        }

        Request req;
        if (!lines.empty())
        {
            std::istringstream rl(lines[0]);
            rl >> req.method >> req.path >> req.protocol;
        }
        if (req.protocol.empty())
            req.protocol = "RTSP/1.0";
        for (size_t i = 1; i < lines.size(); i++)
        {
            const size_t idx = lines[i].find(':');
            if (idx == std::string::npos)
                continue;
            req.headers[lower(trim(lines[i].substr(0, idx)))] = trim(lines[i].substr(idx + 1));
        }

        const size_t bodyStart = headerEnd + 4;
        const size_t contentLength = (size_t)std::strtoul(req.header("content-length").c_str(), nullptr, 10);
        const size_t bodyEnd = bodyStart + contentLength;
        if (bodyEnd > buf.size())
            break; // body not fully received

        req.body.assign(buf.begin() + bodyStart, buf.begin() + bodyEnd);
        out.push_back(std::move(req));
        offset = bodyEnd;
    }

    buf.erase(buf.begin(), buf.begin() + offset);
    return out;
}

Bytes build(const Request &req, const Response &res)
{
    const char *statusText = "OK";
    switch (res.status)
    {
    case 400: statusText = "Bad Request"; break;
    case 404: statusText = "Not Found"; break;
    case 500: statusText = "Internal Server Error"; break;
    default: statusText = "OK"; break;
    }

    std::string head = (req.protocol.empty() ? "RTSP/1.0" : req.protocol) + " " +
                       std::to_string(res.status) + " " + statusText + "\r\n";
    for (const auto &h : res.headers)
        head += h.first + ": " + h.second + "\r\n";
    const std::string cseq = req.header("cseq");
    if (!cseq.empty())
        head += "CSeq: " + cseq + "\r\n";
    head += "Content-Length: " + std::to_string(res.body.size()) + "\r\n\r\n";

    Bytes out(head.begin(), head.end());
    out.insert(out.end(), res.body.begin(), res.body.end());
    return out;
}
} // namespace cp_rtsp
