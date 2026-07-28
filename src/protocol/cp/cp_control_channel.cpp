#include "cp_control_channel.h"

#include <cstdio>
#include <cstdlib>

#include "common/logger.h"

namespace cp_control_channel
{
namespace
{
// Bring-up aid: when FCP_CP_CAPTURE is set, dump each request's body to
// $FCP_CP_CAPTURE/<n>-<METHOD>.bin so the AV plists can be analysed offline.
void captureRequest(const cp_rtsp::Request &req)
{
    const char *dir = getenv("FCP_CP_CAPTURE");
    if (!dir)
        return;
    static int n = 0;
    char path[512];
    std::string m = req.method;
    for (char &c : m) if (c == '/') c = '_';
    snprintf(path, sizeof(path), "%s/%02d-%s.bin", dir, n++, m.c_str());
    if (FILE *f = fopen(path, "wb"))
    {
        if (!req.body.empty())
            fwrite(req.body.data(), 1, req.body.size(), f);
        fclose(f);
    }
    log_i("[cp] AV req %s %s ct=%s body=%zuB -> %s", req.method.c_str(), req.path.c_str(),
          req.header("content-type").c_str(), req.body.size(), path);
}
} // namespace
namespace
{
// Path may carry a query string; compare the path part only.
bool pathIs(const std::string &path, const char *want)
{
    const size_t q = path.find('?');
    return path.compare(0, q == std::string::npos ? path.size() : q, want) == 0;
}

const char *PAIRING_TLV8 = "application/pairing+tlv8";
} // namespace

cp_rtsp::Response ControlChannel::route(const cp_rtsp::Request &req)
{
    cp_rtsp::Response res;

    if (pathIs(req.path, "/pair-setup"))
    {
        res.body = _setup.handle(req.body);
        res.headers["Content-Type"] = PAIRING_TLV8;
        return res;
    }

    if (pathIs(req.path, "/pair-verify"))
    {
        res.body = _verify.handle(req.body);
        res.headers["Content-Type"] = PAIRING_TLV8;
        // On the M4 that completes pair-verify, keep this response plaintext
        // and switch to encrypted framing for everything after it.
        if (_verify.verified() && !_cipher)
            _activateCipherAfterResponse = true;
        return res;
    }

    if (pathIs(req.path, "/auth-setup"))
    {
        if (!_signer)
        {
            res.status = 500;
            return res;
        }
        res.body = cp_auth_setup::handle(req.body, *_signer);
        res.headers["Content-Type"] = "application/octet-stream";
        if (res.body.empty())
            res.status = 400;
        return res;
    }

    // Anything past pairing is the CarPlay AV protocol (GET /info, SETUP,
    // RECORD, POST /command|/feedback, ...). Capture it for bring-up.
    captureRequest(req);
    log_w("[cp] unhandled %s %s", req.method.c_str(), req.path.c_str());
    res.status = 404;
    return res;
}

Bytes ControlChannel::process(const Bytes &incoming)
{
    // 1) Get plaintext RTSP bytes: decrypt first if the channel is encrypted.
    if (_cipher)
    {
        _cipherIn.insert(_cipherIn.end(), incoming.begin(), incoming.end());
        Bytes decrypted;
        if (!_cipher->decrypt(_cipherIn, decrypted))
        {
            log_w("[cp] control-channel decrypt failed");
            return {};
        }
        _plainIn.insert(_plainIn.end(), decrypted.begin(), decrypted.end());
    }
    else
    {
        _plainIn.insert(_plainIn.end(), incoming.begin(), incoming.end());
    }

    // 2) Parse + dispatch each complete request; frame each response.
    Bytes out;
    std::vector<cp_rtsp::Request> requests = cp_rtsp::parse(_plainIn);
    for (const cp_rtsp::Request &req : requests)
    {
        cp_rtsp::Response res = route(req);
        Bytes responseBytes = cp_rtsp::build(req, res);

        if (_cipher)
            responseBytes = _cipher->encrypt(responseBytes);
        out.insert(out.end(), responseBytes.begin(), responseBytes.end());

        // Activate the cipher only after its (plaintext) M4 response is queued.
        if (_activateCipherAfterResponse)
        {
            _cipher = std::make_unique<cp_control_cipher::ControlCipher>(
                _verify.keys().readKey, _verify.keys().writeKey);
            _activateCipherAfterResponse = false;
            log_i("[cp] control channel now encrypted (controller %s)",
                  _verify.controllerId().c_str());
        }
    }
    return out;
}
} // namespace cp_control_channel
