#include "aa_ssl.h"

#include <cstring>

#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/pem.h>

#include "protocol/aa/aa_cert.h"
#include "common/logger.h"
#include "settings.h"

AaSsl::~AaSsl()
{
    reset();
}

bool AaSsl::error(char *err, const char *msg)
{
    unsigned long code = ERR_get_error();
    char detail[128] = {0};
    if (code)
        ERR_error_string_n(code, detail, sizeof(detail));
    if (err)
        snprintf(err, 256, "%s%s%s", msg, code ? " > " : "", detail);
    ERR_clear_error();
    return false;
}

void AaSsl::reset()
{
    std::lock_guard<std::mutex> lock(_mutex);
    _ready = false;
    if (_ssl)
    {
        SSL_free(_ssl); // frees the BIOs too
        _ssl = nullptr;
        _rbio = nullptr;
        _wbio = nullptr;
    }
    if (_ctx)
    {
        SSL_CTX_free(_ctx);
        _ctx = nullptr;
    }
}

bool AaSsl::init(char *err)
{
    reset();
    std::lock_guard<std::mutex> lock(_mutex);

    _ctx = SSL_CTX_new(TLS_client_method());
    if (!_ctx)
        return error(err, "Can't create SSL context");

    // The GAL protocol speaks TLS 1.2; the phone does not verify against a
    // public CA and neither do we.
    SSL_CTX_set_min_proto_version(_ctx, TLS1_2_VERSION);
    SSL_CTX_set_max_proto_version(_ctx, TLS1_2_VERSION);
    SSL_CTX_set_verify(_ctx, SSL_VERIFY_NONE, nullptr);

    // Optionally force ChaCha20-Poly1305: on a CPU without AES acceleration it
    // is far cheaper than AES-128-GCM, but the phone only uses it if we offer
    // *nothing else* (it prefers AES-GCM by server preference). Off by default
    // since a phone that requires AES-GCM for AA would then fail to connect.
    if (Settings::forceChacha20)
        SSL_CTX_set_cipher_list(_ctx, "ECDHE-RSA-CHACHA20-POLY1305");

    BIO *certBio = BIO_new_mem_buf(AA_HU_CERT_PEM, -1);
    X509 *cert = PEM_read_bio_X509(certBio, nullptr, nullptr, nullptr);
    BIO_free(certBio);
    if (!cert || SSL_CTX_use_certificate(_ctx, cert) != 1)
    {
        if (cert)
            X509_free(cert);
        return error(err, "Can't load head unit certificate");
    }
    X509_free(cert);

    BIO *keyBio = BIO_new_mem_buf(AA_HU_KEY_PEM, -1);
    EVP_PKEY *key = PEM_read_bio_PrivateKey(keyBio, nullptr, nullptr, nullptr);
    BIO_free(keyBio);
    if (!key || SSL_CTX_use_PrivateKey(_ctx, key) != 1)
    {
        if (key)
            EVP_PKEY_free(key);
        return error(err, "Can't load head unit private key");
    }
    EVP_PKEY_free(key);

    _ssl = SSL_new(_ctx);
    if (!_ssl)
        return error(err, "Can't create SSL session");

    _rbio = BIO_new(BIO_s_mem());
    _wbio = BIO_new(BIO_s_mem());
    if (!_rbio || !_wbio)
        return error(err, "Can't create memory BIOs");

    SSL_set_bio(_ssl, _rbio, _wbio);
    SSL_set_connect_state(_ssl);
    _ready = true;
    return true;
}

bool AaSsl::drainOut(std::vector<uint8_t> &out)
{
    uint8_t buffer[4096];
    int pending;
    while ((pending = BIO_read(_wbio, buffer, sizeof(buffer))) > 0)
        out.insert(out.end(), buffer, buffer + pending);
    return true;
}

bool AaSsl::handshake(const uint8_t *in, int inLen, std::vector<uint8_t> &out, bool &done, char *err)
{
    std::lock_guard<std::mutex> lock(_mutex);
    done = false;
    if (!_ssl)
        return error(err, "SSL not initialised");

    if (in && inLen > 0 && BIO_write(_rbio, in, inLen) != inLen)
        return error(err, "BIO_write failed");

    int result = SSL_do_handshake(_ssl);
    if (result != 1)
    {
        int reason = SSL_get_error(_ssl, result);
        if (reason != SSL_ERROR_WANT_READ && reason != SSL_ERROR_WANT_WRITE)
        {
            char msg[64];
            snprintf(msg, sizeof(msg), "TLS handshake failed (%d)", reason);
            return error(err, msg);
        }
    }

    drainOut(out);
    done = SSL_is_init_finished(_ssl);
    return true;
}

bool AaSsl::decrypt(const uint8_t *in, int inLen, std::vector<uint8_t> &out, char *err)
{
    std::lock_guard<std::mutex> lock(_mutex);
    if (!_ssl)
        return error(err, "SSL not initialised");

    if (BIO_write(_rbio, in, inLen) != inLen)
        return error(err, "BIO_write failed");

    uint8_t buffer[16 * 1024];
    int bytes;
    while ((bytes = SSL_read(_ssl, buffer, sizeof(buffer))) > 0)
        out.insert(out.end(), buffer, buffer + bytes);

    int reason = SSL_get_error(_ssl, bytes);
    if (reason != SSL_ERROR_WANT_READ && reason != SSL_ERROR_WANT_WRITE && reason != SSL_ERROR_NONE)
    {
        char msg[64];
        snprintf(msg, sizeof(msg), "TLS decrypt failed (%d)", reason);
        return error(err, msg);
    }
    return true;
}

bool AaSsl::encrypt(const uint8_t *in, int inLen, std::vector<uint8_t> &out, char *err)
{
    std::lock_guard<std::mutex> lock(_mutex);
    if (!_ssl)
        return error(err, "SSL not initialised");

    int written = SSL_write(_ssl, in, inLen);
    if (written != inLen)
    {
        char msg[64];
        snprintf(msg, sizeof(msg), "TLS encrypt failed (%d)", SSL_get_error(_ssl, written));
        return error(err, msg);
    }

    drainOut(out);
    return true;
}

const std::string AaSsl::cipherName() const
{
    if (!_ssl)
        return "none";
    const SSL_CIPHER *cipher = SSL_get_current_cipher(_ssl);
    return cipher ? SSL_CIPHER_get_name(cipher) : "none";
}
