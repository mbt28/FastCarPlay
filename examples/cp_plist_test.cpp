// Binary plist codec tests: round-trip a nested value, and decode a real
// captured CarPlay SETUP plist (pass its path as argv[1]) to validate against
// the on-wire format the iPhone sends.
//
//   make cp_plist_test && ../out/cp_plist_test [/tmp/cpav/00-SETUP.bin]

#include <cstdio>
#include <fstream>

#include "protocol/cp/cp_plist.h"

using cp_plist::Value;

static int failures = 0;
static void check(bool ok, const char *what)
{
    printf("  %-56s %s\n", what, ok ? "ok" : "FAILED");
    if (!ok) failures++;
}

static void dump(const Value &v, int indent)
{
    std::string pad(indent * 2, ' ');
    switch (v.type)
    {
    case Value::Type::Bool: printf("%s\n", v.b ? "true" : "false"); break;
    case Value::Type::Int: printf("%lld\n", (long long)v.i); break;
    case Value::Type::Real: printf("%f\n", v.r); break;
    case Value::Type::Str: printf("\"%s\"\n", v.s.c_str()); break;
    case Value::Type::Data: printf("<%zuB>\n", v.data.size()); break;
    case Value::Type::Null: printf("null\n"); break;
    case Value::Type::Array:
        printf("[\n");
        for (const auto &c : v.array) { printf("%s  ", pad.c_str()); dump(c, indent + 1); }
        printf("%s]\n", pad.c_str());
        break;
    case Value::Type::Dict:
        printf("{\n");
        for (const auto &kv : v.dict) { printf("%s  %s = ", pad.c_str(), kv.first.c_str()); dump(kv.second, indent + 1); }
        printf("%s}\n", pad.c_str());
        break;
    }
}

int main(int argc, char **argv)
{
    printf("binary plist codec\n\n");

    printf("round-trip:\n");
    {
        Value root = Value::map();
        root.set("timingPort", Value::integer(56531));
        root.set("eventPort", Value::integer(49152));
        root.set("name", Value::str(u8"Mehmet’s iPhone")); // non-ASCII -> UTF-16
        root.set("keepAlive", Value::boolean(true));
        Value feats = Value::arr();
        feats.array.push_back(Value::str("hevc"));
        feats.array.push_back(Value::str("iAPChannel"));
        feats.array.push_back(Value::str("viewAreas"));
        root.set("enabledFeatures", feats);
        Value nested = Value::map();
        nested.set("type", Value::integer(110));
        nested.set("blob", Value::bytes({1, 2, 3, 4, 5}));
        root.set("stream", nested);

        cp_plist::Bytes enc = cp_plist::encode(root);
        check(enc.size() > 8 && std::string(enc.begin(), enc.begin() + 8) == "bplist00", "encodes bplist00");

        Value dec;
        check(cp_plist::decode(enc, dec), "decodes back");
        check(dec.intOr("timingPort") == 56531 && dec.intOr("eventPort") == 49152, "ints round-trip");
        check(dec.strOr("name") == u8"Mehmet’s iPhone", "unicode string round-trips");
        const Value *f = dec.find("enabledFeatures");
        check(f && f->array.size() == 3 && f->array[0].s == "hevc", "array round-trips");
        const Value *st = dec.find("stream");
        check(st && st->intOr("type") == 110, "nested dict round-trips");
        check(st && st->find("blob")->data == cp_plist::Bytes{1, 2, 3, 4, 5}, "data round-trips");
    }

    const char *path = argc > 1 ? argv[1] : "/tmp/cpav/00-SETUP.bin";
    printf("\nreal captured SETUP (%s):\n", path);
    {
        std::ifstream in(path, std::ios::binary);
        if (!in)
        {
            printf("  (not found -- skipping; capture one with FCP_CP_CAPTURE)\n");
        }
        else
        {
            cp_plist::Bytes buf((std::istreambuf_iterator<char>(in)), {});
            Value v;
            check(cp_plist::decode(buf, v), "decodes the real SETUP plist");
            check(v.type == Value::Type::Dict, "top-level is a dict");
            check(v.find("deviceID") != nullptr, "has deviceID");
            printf("  model=%s os=%s timingPort=%lld\n", v.strOr("model").c_str(),
                   v.strOr("osVersion").c_str(), (long long)v.intOr("timingPort"));
            // Re-encode and decode to confirm our encoder produces parseable output.
            Value again;
            check(cp_plist::decode(cp_plist::encode(v), again) && again.strOr("model") == v.strOr("model"),
                  "re-encode -> decode preserves fields");
            if (argc > 2) dump(v, 0);
        }
    }

    printf("\n%s\n", failures == 0 ? "PASS" : "FAIL");
    return failures == 0 ? 0 : 1;
}
