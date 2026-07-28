#include "cp_identity.h"

#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>

#include <sys/stat.h>
#include <openssl/rand.h>

#include "cp_crypto.h"

namespace cp_identity
{
namespace
{
std::string g_dir;
Identity g_cached;
bool g_loaded = false;

std::string storageDir()
{
    if (!g_dir.empty())
        return g_dir;
    const char *home = getenv("HOME");
    if (home == nullptr || *home == '\0')
        home = "/root";
    return std::string(home) + "/.fastcarplay/cp";
}

void ensureDir(const std::string &dir)
{
    // Create parents as needed (two levels: ~/.fastcarplay and .../cp).
    for (size_t i = 1; i <= dir.size(); i++)
        if (i == dir.size() || dir[i] == '/')
            mkdir(dir.substr(0, i).c_str(), 0700);
}

std::string toHex(const Bytes &b)
{
    static const char *hex = "0123456789abcdef";
    std::string s;
    s.reserve(b.size() * 2);
    for (uint8_t x : b)
    {
        s.push_back(hex[x >> 4]);
        s.push_back(hex[x & 0xf]);
    }
    return s;
}

Bytes fromHex(const std::string &s)
{
    Bytes b;
    auto nib = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return 0;
    };
    for (size_t i = 0; i + 1 < s.size(); i += 2)
        b.push_back((uint8_t)(nib(s[i]) << 4 | nib(s[i + 1])));
    return b;
}

std::string randomUuid()
{
    uint8_t r[16];
    RAND_bytes(r, sizeof(r));
    r[6] = (r[6] & 0x0f) | 0x40; // version 4
    r[8] = (r[8] & 0x3f) | 0x80; // variant
    char buf[37];
    snprintf(buf, sizeof(buf),
             "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             r[0], r[1], r[2], r[3], r[4], r[5], r[6], r[7], r[8], r[9], r[10], r[11],
             r[12], r[13], r[14], r[15]);
    return buf;
}
} // namespace

void setStorageDir(const std::string &dir)
{
    g_dir = dir;
    g_loaded = false;
}

const Identity &loadOrCreateIdentity()
{
    if (g_loaded)
        return g_cached;

    const std::string file = storageDir() + "/identity.txt";
    std::ifstream in(file);
    if (in.is_open())
    {
        std::string line, priv, pub, pi;
        while (std::getline(in, line))
        {
            const size_t eq = line.find('=');
            if (eq == std::string::npos)
                continue;
            const std::string key = line.substr(0, eq), val = line.substr(eq + 1);
            if (key == "priv") priv = val;
            else if (key == "pub") pub = val;
            else if (key == "pi") pi = val;
        }
        if (priv.size() == 64 && pub.size() == 64 && !pi.empty())
        {
            g_cached = {fromHex(priv), fromHex(pub), pi};
            g_loaded = true;
            return g_cached;
        }
    }

    // First run: generate and persist.
    cp_crypto::Ed25519Pair kp = cp_crypto::ed25519Generate();
    g_cached = {kp.privRaw, kp.pubRaw, randomUuid()};
    g_loaded = true;

    ensureDir(storageDir());
    std::ofstream out(file, std::ios::trunc);
    if (out.is_open())
        out << "priv=" << toHex(g_cached.privRaw) << "\n"
            << "pub=" << toHex(g_cached.pubRaw) << "\n"
            << "pi=" << g_cached.pairingId << "\n";
    chmod(file.c_str(), 0600);
    return g_cached;
}

void savePairing(const std::string &identifier, const Bytes &ltpk)
{
    const std::string file = storageDir() + "/pairings.txt";

    // Read existing, replace/add this identifier, write back.
    std::ostringstream merged;
    bool replaced = false;
    std::ifstream in(file);
    std::string line;
    while (std::getline(in, line))
    {
        const size_t sp = line.find(' ');
        if (sp == std::string::npos)
            continue;
        if (line.substr(0, sp) == identifier)
        {
            merged << identifier << ' ' << toHex(ltpk) << "\n";
            replaced = true;
        }
        else
            merged << line << "\n";
    }
    if (!replaced)
        merged << identifier << ' ' << toHex(ltpk) << "\n";

    ensureDir(storageDir());
    std::ofstream out(file, std::ios::trunc);
    if (out.is_open())
        out << merged.str();
    chmod(file.c_str(), 0600);
}

bool getPairing(const std::string &identifier, Bytes &ltpk)
{
    std::ifstream in(storageDir() + "/pairings.txt");
    std::string line;
    while (std::getline(in, line))
    {
        const size_t sp = line.find(' ');
        if (sp == std::string::npos)
            continue;
        if (line.substr(0, sp) == identifier)
        {
            ltpk = fromHex(line.substr(sp + 1));
            return !ltpk.empty();
        }
    }
    return false;
}
} // namespace cp_identity
