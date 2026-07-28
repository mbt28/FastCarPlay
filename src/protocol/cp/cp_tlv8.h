#ifndef SRC_PROTOCOL_CP_CP_TLV8
#define SRC_PROTOCOL_CP_CP_TLV8

// TLV8 codec for the CarPlay/HomeKit pairing messages. Each item is
// [1B type][1B length][value]; values over 255 bytes are split into
// consecutive same-type items and rejoined on decode. Mirrors LIVI tlv8.ts.

#include <cstdint>
#include <map>
#include <vector>

namespace cp_tlv8
{
using Bytes = std::vector<uint8_t>;

struct Item
{
    uint8_t type;
    Bytes value;
};

Bytes encode(const std::vector<Item> &items);

// type -> value, consecutive same-type fragments joined. Later duplicate
// (non-fragment) types overwrite earlier ones.
std::map<uint8_t, Bytes> decode(const Bytes &buf);
} // namespace cp_tlv8

#endif /* SRC_PROTOCOL_CP_CP_TLV8 */
