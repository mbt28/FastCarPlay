#ifndef SRC_PROTOCOL_AA_AA_SSL
#define SRC_PROTOCOL_AA_AA_SSL

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include <openssl/ssl.h>

// TLS client over OpenSSL memory BIOs for the Android Auto link. The
// handshake and all records travel inside AA frames, not on a socket:
// ciphertext in via writeCipher(), ciphertext out via readCipher().
// Thread safety: one mutex guards the single SSL object, so decrypt()
// (process thread) and encrypt() (write thread) may be called concurrently.
class AaSsl
{
public:
    AaSsl() = default;
    ~AaSsl();

    AaSsl(const AaSsl &) = delete;
    AaSsl &operator=(const AaSsl &) = delete;

    // Create SSL_CTX/SSL with the embedded head-unit client certificate.
    bool init(char *err = nullptr);
    void reset();

    bool ready() const { return _ready; }

    // Drive the handshake: feed the phone's ciphertext (may be empty for the
    // first step), collect ours into `out`. Returns false on fatal error;
    // sets `done` once SSL_is_init_finished.
    bool handshake(const uint8_t *in, int inLen, std::vector<uint8_t> &out, bool &done, char *err = nullptr);

    // Post-handshake record processing.
    // decrypt: ciphertext in -> plaintext appended to `out`.
    bool decrypt(const uint8_t *in, int inLen, std::vector<uint8_t> &out, char *err = nullptr);
    // encrypt: plaintext in -> ciphertext appended to `out`.
    bool encrypt(const uint8_t *in, int inLen, std::vector<uint8_t> &out, char *err = nullptr);

    const std::string cipherName() const;

private:
    bool drainOut(std::vector<uint8_t> &out);
    static bool error(char *err, const char *msg);

    std::mutex _mutex;
    SSL_CTX *_ctx = nullptr;
    SSL *_ssl = nullptr;
    BIO *_rbio = nullptr; // ciphertext from the phone -> SSL
    BIO *_wbio = nullptr; // ciphertext from SSL -> the phone
    bool _ready = false;
};

#endif /* SRC_PROTOCOL_AA_AA_SSL */
