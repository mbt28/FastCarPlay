#ifndef SRC_PROTOCOL_CP_CP_HID
#define SRC_PROTOCOL_CP_CP_HID

// CarPlay HID input reports. The accessory declares HID devices (touchscreen,
// knob, media, telephony) in /info and delivers input to the phone as HID
// reports wrapped in a `hidSendReport` event-channel command. This builds the
// report bytes for each device from its descriptor layout (see cp_av handleInfo
// / LIVI cp/stack/hid.ts). The uuids match the "uuid" fields advertised there.

#include <cstdint>
#include <vector>

namespace cp_hid
{
using Bytes = std::vector<uint8_t>;

// HID device uuids (hex strings) as advertised in /info hidDevices.
constexpr const char *TOUCH_UUID = "2a2a2a2a";
constexpr const char *KNOB_UUID = "2a2a2a2b";
constexpr const char *MEDIA_UUID = "2a2a2a2c";
constexpr const char *TELEPHONY_UUID = "2a2a2a2d";

// Simultaneous touch contacts the descriptor declares (2 covers pinch/rotate).
constexpr int TOUCH_CONTACTS = 2;

// Consumer/media key indices (mediaReport). 0 releases.
enum Media
{
    MEDIA_NONE = 0,
    MEDIA_PLAY = 1,
    MEDIA_PAUSE = 2,
    MEDIA_PLAY_PAUSE = 3,
    MEDIA_NEXT = 4,
    MEDIA_PREV = 5,
    MEDIA_NAV_GUIDANCE = 6,
};

struct Contact
{
    int slot; // fixed transducer index / finger slot (0..TOUCH_CONTACTS-1)
    int x;    // absolute pixels
    int y;
    bool down;
};

// Multitouch report: TOUCH_CONTACTS slots of [index][touch(+pad)][X LE16][Y LE16].
// Slots not in `contacts` are emitted idle (index set, touch 0).
Bytes touchReport(const std::vector<Contact> &contacts);
// Single-finger convenience (slot 0 active, slot 1 idle).
Bytes touchReport(int x, int y, bool down);

// Consumer-control (media) report: one array byte = the pressed usage index.
Bytes mediaReport(int index);

// Knob report: [buttons(select 0x01|home 0x02|back 0x04)][X int8][Y int8][wheel int8].
Bytes knobReport(bool select, bool home, bool back, int x = 0, int y = 0, int wheel = 0);

} // namespace cp_hid

#endif /* SRC_PROTOCOL_CP_CP_HID */
