#include "cp_plist.h"

#include <cstring>

namespace cp_plist
{
namespace
{
// ── Encoding ────────────────────────────────────────────────────────────
// bplist00 = "bplist00" + object bodies + offset table + 32-byte trailer.
// Objects reference each other by index; containers store child indices using
// objectRefSize bytes. We flatten the tree into an ordered object list first so
// indices (and thus the ref size) are known before serialization.

struct LObj
{
    int kind; // 0 bool,1 int,2 real,3 ascii,4 utf16,5 data,6 array,7 dict,8 null
    bool b = false;
    int64_t i = 0;
    double r = 0;
    std::string s;
    Bytes data;
    std::vector<int> arr;                 // array child indices
    std::vector<std::pair<int, int>> map; // dict (keyIdx, valIdx)
};

bool isAscii(const std::string &s)
{
    for (unsigned char c : s)
        if (c >= 0x80)
            return false;
    return true;
}

// Append value (and its children) to the flat list; return its object index.
int flatten(const Value &v, std::vector<LObj> &objs)
{
    LObj o;
    switch (v.type)
    {
    case Value::Type::Null: o.kind = 8; break;
    case Value::Type::Bool: o.kind = 0; o.b = v.b; break;
    case Value::Type::Int: o.kind = 1; o.i = v.i; break;
    case Value::Type::Real: o.kind = 2; o.r = v.r; break;
    case Value::Type::Str: o.kind = isAscii(v.s) ? 3 : 4; o.s = v.s; break;
    case Value::Type::Data: o.kind = 5; o.data = v.data; break;
    case Value::Type::Array: o.kind = 6; break;
    case Value::Type::Dict: o.kind = 7; break;
    }
    const int idx = (int)objs.size();
    objs.push_back(o);

    if (v.type == Value::Type::Array)
    {
        std::vector<int> refs;
        refs.reserve(v.array.size());
        for (const Value &c : v.array)
            refs.push_back(flatten(c, objs));
        objs[idx].arr = std::move(refs);
    }
    else if (v.type == Value::Type::Dict)
    {
        std::vector<std::pair<int, int>> refs;
        refs.reserve(v.dict.size());
        for (const auto &kv : v.dict)
        {
            Value key = Value::str(kv.first);
            const int k = flatten(key, objs);
            const int val = flatten(kv.second, objs);
            refs.emplace_back(k, val);
        }
        objs[idx].map = std::move(refs);
    }
    return idx;
}

void putBE(Bytes &b, uint64_t v, int nbytes)
{
    for (int i = nbytes - 1; i >= 0; i--)
        b.push_back((uint8_t)((v >> (8 * i)) & 0xff));
}

// Marker byte 0xTL with an extended int-length object when length >= 15.
void putTypeAndLength(Bytes &b, uint8_t type, size_t len)
{
    if (len < 15)
    {
        b.push_back((uint8_t)((type << 4) | len));
        return;
    }
    b.push_back((uint8_t)((type << 4) | 0x0f));
    // Length as an int object: 0x1X + big-endian bytes.
    if (len <= 0xff) { b.push_back(0x10); putBE(b, len, 1); }
    else if (len <= 0xffff) { b.push_back(0x11); putBE(b, len, 2); }
    else { b.push_back(0x12); putBE(b, (uint32_t)len, 4); }
}

void encodeObj(const LObj &o, Bytes &b, int refSize)
{
    switch (o.kind)
    {
    case 8: b.push_back(0x00); break;             // null
    case 0: b.push_back(o.b ? 0x09 : 0x08); break; // bool
    case 1:                                         // int
    {
        uint64_t u = (uint64_t)o.i;
        if (o.i >= 0 && o.i <= 0xff) { b.push_back(0x10); putBE(b, u, 1); }
        else if (o.i >= 0 && o.i <= 0xffff) { b.push_back(0x11); putBE(b, u, 2); }
        else if (o.i >= 0 && o.i <= 0xffffffffLL) { b.push_back(0x12); putBE(b, u, 4); }
        else { b.push_back(0x13); putBE(b, u, 8); } // 8-byte (also negatives)
        break;
    }
    case 2: // real (double)
    {
        b.push_back(0x23);
        uint64_t bits;
        std::memcpy(&bits, &o.r, 8);
        putBE(b, bits, 8);
        break;
    }
    case 3: // ASCII string
        putTypeAndLength(b, 0x5, o.s.size());
        b.insert(b.end(), o.s.begin(), o.s.end());
        break;
    case 4: // UTF-16BE string
    {
        // Minimal UTF-8 -> UTF-16BE (BMP only; CarPlay strings are ASCII/BMP).
        std::vector<uint16_t> units;
        for (size_t i = 0; i < o.s.size();)
        {
            uint8_t c = o.s[i];
            uint32_t cp;
            if (c < 0x80) { cp = c; i += 1; }
            else if ((c >> 5) == 0x6 && i + 1 < o.s.size()) { cp = ((c & 0x1f) << 6) | (o.s[i + 1] & 0x3f); i += 2; }
            else if ((c >> 4) == 0xe && i + 2 < o.s.size()) { cp = ((c & 0x0f) << 12) | ((o.s[i + 1] & 0x3f) << 6) | (o.s[i + 2] & 0x3f); i += 3; }
            else { cp = 0xfffd; i += 1; }
            units.push_back((uint16_t)(cp <= 0xffff ? cp : 0xfffd));
        }
        putTypeAndLength(b, 0x6, units.size());
        for (uint16_t u : units) { b.push_back(u >> 8); b.push_back(u & 0xff); }
        break;
    }
    case 5: // data
        putTypeAndLength(b, 0x4, o.data.size());
        b.insert(b.end(), o.data.begin(), o.data.end());
        break;
    case 6: // array
        putTypeAndLength(b, 0xA, o.arr.size());
        for (int ref : o.arr) putBE(b, (uint64_t)ref, refSize);
        break;
    case 7: // dict
        putTypeAndLength(b, 0xD, o.map.size());
        for (const auto &kv : o.map) putBE(b, (uint64_t)kv.first, refSize);
        for (const auto &kv : o.map) putBE(b, (uint64_t)kv.second, refSize);
        break;
    }
}

int byteWidth(uint64_t maxVal)
{
    if (maxVal <= 0xff) return 1;
    if (maxVal <= 0xffff) return 2;
    if (maxVal <= 0xffffffffULL) return 4;
    return 8;
}
} // namespace

Bytes encode(const Value &root)
{
    std::vector<LObj> objs;
    flatten(root, objs);
    const size_t count = objs.size();
    const int refSize = byteWidth(count == 0 ? 0 : count - 1);

    Bytes out = {'b', 'p', 'l', 'i', 's', 't', '0', '0'};
    std::vector<uint64_t> offsets(count);
    for (size_t k = 0; k < count; k++)
    {
        offsets[k] = out.size();
        encodeObj(objs[k], out, refSize);
    }

    const uint64_t offsetTableOffset = out.size();
    const int offsetSize = byteWidth(offsetTableOffset);
    for (uint64_t off : offsets)
        putBE(out, off, offsetSize);

    // 32-byte trailer.
    for (int i = 0; i < 6; i++) out.push_back(0); // unused
    out.push_back(0);                             // sort version
    out.push_back((uint8_t)offsetSize);
    out.push_back((uint8_t)refSize);
    putBE(out, count, 8);
    putBE(out, 0, 8);                 // top object index
    putBE(out, offsetTableOffset, 8);
    return out;
}

// ── Decoding ────────────────────────────────────────────────────────────
namespace
{
struct Decoder
{
    const Bytes &b;
    int offsetSize = 0;
    int refSize = 0;
    uint64_t count = 0;
    uint64_t tableOffset = 0;
    bool ok = true;

    explicit Decoder(const Bytes &buf) : b(buf) {}

    uint64_t getBE(size_t pos, int nbytes)
    {
        if (pos + nbytes > b.size()) { ok = false; return 0; }
        uint64_t v = 0;
        for (int i = 0; i < nbytes; i++)
            v = (v << 8) | b[pos + i];
        return v;
    }

    uint64_t offsetOf(uint64_t index)
    {
        if (index >= count) { ok = false; return 0; }
        return getBE(tableOffset + index * offsetSize, offsetSize);
    }

    // Read a type/length: returns the low-nibble length, resolving the extended
    // int-length form. Advances pos past the marker (+ extended int).
    uint64_t readLength(size_t &pos, uint8_t low)
    {
        if (low != 0x0f)
            return low;
        if (pos >= b.size()) { ok = false; return 0; }
        uint8_t im = b[pos++];
        if ((im >> 4) != 0x1) { ok = false; return 0; }
        int n = 1 << (im & 0x0f);
        uint64_t len = getBE(pos, n);
        pos += n;
        return len;
    }

    Value parse(uint64_t index)
    {
        Value out;
        if (!ok || index >= count) { ok = false; return out; }
        size_t pos = offsetOf(index);
        if (pos >= b.size()) { ok = false; return out; }
        uint8_t marker = b[pos++];
        uint8_t hi = marker >> 4, lo = marker & 0x0f;

        switch (hi)
        {
        case 0x0:
            if (marker == 0x08) out = Value::boolean(false);
            else if (marker == 0x09) out = Value::boolean(true);
            else out.type = Value::Type::Null;
            break;
        case 0x1: // int
        {
            int n = 1 << lo;
            uint64_t v = getBE(pos, n);
            // 8-byte ints may be negative (two's complement).
            out = Value::integer(n == 8 ? (int64_t)v : (int64_t)v);
            break;
        }
        case 0x2: // real
        {
            if (lo == 3) { uint64_t bits = getBE(pos, 8); double d; std::memcpy(&d, &bits, 8); out = Value::real(d); }
            else if (lo == 2) { uint32_t bits = (uint32_t)getBE(pos, 4); float f; std::memcpy(&f, &bits, 4); out = Value::real(f); }
            else ok = false;
            break;
        }
        case 0x4: // data
        {
            uint64_t len = readLength(pos, lo);
            if (pos + len > b.size()) { ok = false; break; }
            out = Value::bytes(Bytes(b.begin() + pos, b.begin() + pos + len));
            break;
        }
        case 0x5: // ASCII string
        {
            uint64_t len = readLength(pos, lo);
            if (pos + len > b.size()) { ok = false; break; }
            out = Value::str(std::string(b.begin() + pos, b.begin() + pos + len));
            break;
        }
        case 0x6: // UTF-16BE string
        {
            uint64_t units = readLength(pos, lo);
            std::string s;
            for (uint64_t k = 0; k < units; k++)
            {
                uint16_t u = (uint16_t)getBE(pos + k * 2, 2);
                // BMP -> UTF-8 (CarPlay strings stay in the BMP).
                if (u < 0x80) s.push_back((char)u);
                else if (u < 0x800) { s.push_back((char)(0xc0 | (u >> 6))); s.push_back((char)(0x80 | (u & 0x3f))); }
                else { s.push_back((char)(0xe0 | (u >> 12))); s.push_back((char)(0x80 | ((u >> 6) & 0x3f))); s.push_back((char)(0x80 | (u & 0x3f))); }
            }
            out = Value::str(std::move(s));
            break;
        }
        case 0xA: // array
        {
            uint64_t n = readLength(pos, lo);
            out = Value::arr();
            for (uint64_t k = 0; k < n; k++)
            {
                uint64_t ref = getBE(pos + k * refSize, refSize);
                out.array.push_back(parse(ref));
            }
            break;
        }
        case 0xD: // dict
        {
            uint64_t n = readLength(pos, lo);
            out = Value::map();
            for (uint64_t k = 0; k < n; k++)
            {
                uint64_t keyRef = getBE(pos + k * refSize, refSize);
                uint64_t valRef = getBE(pos + (n + k) * refSize, refSize);
                Value key = parse(keyRef);
                Value val = parse(valRef);
                out.dict.emplace_back(key.type == Value::Type::Str ? key.s : std::string(), std::move(val));
            }
            break;
        }
        default: ok = false; break;
        }
        return out;
    }
};
} // namespace

bool decode(const Bytes &buf, Value &out)
{
    if (buf.size() < 8 + 32 || std::memcmp(buf.data(), "bplist00", 8) != 0)
        return false;
    Decoder d(buf);
    const size_t trailer = buf.size() - 32;
    d.offsetSize = buf[trailer + 6];
    d.refSize = buf[trailer + 7];
    d.count = d.getBE(trailer + 8, 8);
    uint64_t top = d.getBE(trailer + 16, 8);
    d.tableOffset = d.getBE(trailer + 24, 8);
    if (!d.ok || d.offsetSize == 0 || d.refSize == 0 || d.count == 0)
        return false;
    out = d.parse(top);
    return d.ok;
}
} // namespace cp_plist
