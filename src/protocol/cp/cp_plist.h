#ifndef SRC_PROTOCOL_CP_CP_PLIST
#define SRC_PROTOCOL_CP_CP_PLIST

// Apple binary property list (bplist00) codec -- the payload format of the
// CarPlay AV control messages (GET /info, SETUP, RECORD, /command, /feedback).
// The phone sends a bplist body and expects one back
// (Content-Type: application/x-apple-binary-plist). This supports the value
// types CarPlay uses: bool, integer, real, string (ASCII/UTF-16BE), data,
// array, and (ordered) dictionary. Mirrors LIVI cp/stack/bplist.ts.

#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace cp_plist
{
using Bytes = std::vector<uint8_t>;

struct Value
{
    enum class Type
    {
        Null,
        Bool,
        Int,
        Real,
        Str,
        Data,
        Array,
        Dict
    } type = Type::Null;

    bool b = false;
    int64_t i = 0;
    double r = 0;
    std::string s;
    Bytes data;
    std::vector<Value> array;
    // Ordered key/value pairs (order preserved on encode).
    std::vector<std::pair<std::string, Value>> dict;

    Value() = default;
    static Value boolean(bool v) { Value x; x.type = Type::Bool; x.b = v; return x; }
    static Value integer(int64_t v) { Value x; x.type = Type::Int; x.i = v; return x; }
    static Value real(double v) { Value x; x.type = Type::Real; x.r = v; return x; }
    static Value str(std::string v) { Value x; x.type = Type::Str; x.s = std::move(v); return x; }
    static Value bytes(Bytes v) { Value x; x.type = Type::Data; x.data = std::move(v); return x; }
    static Value arr() { Value x; x.type = Type::Array; return x; }
    static Value map() { Value x; x.type = Type::Dict; return x; }

    // Dict helpers.
    void set(const std::string &key, Value v) { dict.emplace_back(key, std::move(v)); }
    const Value *find(const std::string &key) const
    {
        for (const auto &kv : dict)
            if (kv.first == key)
                return &kv.second;
        return nullptr;
    }
    // Convenience typed getters (return a default if missing / wrong type).
    int64_t intOr(const std::string &key, int64_t def = 0) const
    {
        const Value *v = find(key);
        return v && v->type == Type::Int ? v->i : def;
    }
    std::string strOr(const std::string &key, const std::string &def = "") const
    {
        const Value *v = find(key);
        return v && v->type == Type::Str ? v->s : def;
    }
};

// Encode a value tree to a bplist00 buffer.
Bytes encode(const Value &root);

// Decode a bplist00 buffer. Returns false on malformed input.
bool decode(const Bytes &buf, Value &out);
} // namespace cp_plist

#endif /* SRC_PROTOCOL_CP_CP_PLIST */
