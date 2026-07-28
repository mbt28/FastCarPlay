#include "cp_tlv8.h"

namespace cp_tlv8
{
Bytes encode(const std::vector<Item> &items)
{
    Bytes out;
    bool havePrev = false;
    uint8_t prevType = 0;

    for (const Item &item : items)
    {
        // A zero-length separator between adjacent same-type items stops the
        // decoder merging them as one fragmented value.
        if (havePrev && prevType == item.type)
        {
            out.push_back(0xff);
            out.push_back(0x00);
        }

        size_t off = 0;
        do
        {
            const size_t len = std::min<size_t>(255, item.value.size() - off);
            out.push_back(item.type);
            out.push_back((uint8_t)len);
            out.insert(out.end(), item.value.begin() + off, item.value.begin() + off + len);
            off += len;
        } while (off < item.value.size());

        prevType = item.type;
        havePrev = true;
    }
    return out;
}

std::map<uint8_t, Bytes> decode(const Bytes &buf)
{
    std::map<uint8_t, Bytes> out;
    size_t p = 0;
    bool haveLast = false;
    uint8_t lastType = 0;
    size_t lastLen = 0;

    while (p + 2 <= buf.size())
    {
        const uint8_t type = buf[p];
        const size_t len = buf[p + 1];
        if (p + 2 + len > buf.size())
            break; // truncated
        const auto begin = buf.begin() + p + 2;
        const auto end = begin + len;
        p += 2 + len;

        // Fragmentation continues only when the previous item was a full 255.
        if (haveLast && type == lastType && lastLen == 255)
            out[type].insert(out[type].end(), begin, end);
        else
            out[type].assign(begin, end);

        lastType = type;
        lastLen = len;
        haveLast = true;
    }
    return out;
}
} // namespace cp_tlv8
