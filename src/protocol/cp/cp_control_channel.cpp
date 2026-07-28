#include "cp_control_channel.h"

#include "common/logger.h"

namespace cp_control_channel
{
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
