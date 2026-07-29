#include "cp_hid.h"

namespace cp_hid
{
namespace
{
constexpr int BYTES_PER_FINGER = 6; // index(8) + touch(1)+pad(7) + X(16) + Y(16)

uint8_t clamp8(int v)
{
    if (v < -127)
        v = -127;
    if (v > 127)
        v = 127;
    return (uint8_t)(int8_t)v;
}
} // namespace

Bytes touchReport(const std::vector<Contact> &contacts)
{
    Bytes r(BYTES_PER_FINGER * TOUCH_CONTACTS, 0);
    // Each slot always reports its fixed transducer index; the phone tracks a
    // finger by the slot it consistently appears in.
    for (int i = 0; i < TOUCH_CONTACTS; i++)
        r[i * BYTES_PER_FINGER] = (uint8_t)i;
    for (const Contact &c : contacts)
    {
        if (c.slot < 0 || c.slot >= TOUCH_CONTACTS)
            continue;
        const int off = c.slot * BYTES_PER_FINGER;
        const int x = c.x < 0 ? 0 : c.x;
        const int y = c.y < 0 ? 0 : c.y;
        r[off] = (uint8_t)c.slot;
        r[off + 1] = c.down ? 0x01 : 0x00;
        r[off + 2] = (uint8_t)(x & 0xff);
        r[off + 3] = (uint8_t)((x >> 8) & 0xff);
        r[off + 4] = (uint8_t)(y & 0xff);
        r[off + 5] = (uint8_t)((y >> 8) & 0xff);
    }
    return r;
}

Bytes touchReport(int x, int y, bool down)
{
    return touchReport(std::vector<Contact>{{0, x, y, down}});
}

Bytes mediaReport(int index)
{
    return Bytes{(uint8_t)(index & 0xff)};
}

Bytes knobReport(bool select, bool home, bool back, int x, int y, int wheel)
{
    return Bytes{(uint8_t)((select ? 0x01 : 0) | (home ? 0x02 : 0) | (back ? 0x04 : 0)), clamp8(x),
                 clamp8(y), clamp8(wheel)};
}

} // namespace cp_hid
