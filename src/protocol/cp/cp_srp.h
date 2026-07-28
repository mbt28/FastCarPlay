#ifndef SRC_PROTOCOL_CP_CP_SRP
#define SRC_PROTOCOL_CP_CP_SRP

// SRP-6a server (RFC 5054 3072-bit group, SHA-512) -- the HAP/AirPlay-2
// pair-setup variant CarPlay uses. Username is the literal "Pair-Setup", the
// password is the setup code ("3939"). Mirrors LIVI srp.ts. Standard SRP-6a
// over OpenSSL BIGNUMs. One-time per phone, off any hot path.

#include <cstdint>
#include <string>
#include <vector>

namespace cp_srp
{
using Bytes = std::vector<uint8_t>;

class Server
{
public:
    Server() = default;

    // Begin a session; fills salt (16B) and the server public key B (384B).
    bool start(const std::string &username, const std::string &password);
    const Bytes &salt() const { return _salt; }
    const Bytes &B() const { return _B; }

    // Verify the client public key A and proof M1. On success fills the shared
    // key K and the server proof M2 (both 64B) and returns true.
    bool verify(const Bytes &A, const Bytes &clientM1, Bytes &K, Bytes &serverM2);

private:
    std::string _username;
    Bytes _salt;
    Bytes _B;
    Bytes _b;    // server private exponent
    Bytes _v;    // verifier
};
} // namespace cp_srp

#endif /* SRC_PROTOCOL_CP_CP_SRP */
