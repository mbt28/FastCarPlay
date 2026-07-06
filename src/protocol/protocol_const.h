#ifndef SRC_PROTOCOL_PROTOCOL_CONST
#define SRC_PROTOCOL_PROTOCOL_CONST

#include <cstdint>

// Carlinkit dongle identification. The dongle is auto-detected by scanning for
// this vendor id and any of the known product ids (0x1520 / 0x1521 depending on
// the model/firmware), so no settings entry is needed. Extend CARLINKIT_PIDS if
// a new dongle model appears, or pin one via the "product-id" setting.
#define CARLINKIT_VID 0x1314
static const uint16_t CARLINKIT_PIDS[] = {0x1520, 0x1521};

#define PROTOCOL_STATUS_UNKNOWN -1     // Manual > 0
#define PROTOCOL_STATUS_INITIALISING 0 // Initialised > 1
#define PROTOCOL_STATUS_NO_DEVICE 1    // Start linking > 3
#define PROTOCOL_STATUS_LINKING 2      // Linked > 4, Failed in sequence > 3
#define PROTOCOL_STATUS_ERROR 3        // Linked > 4, no device in sequence > 1
#define PROTOCOL_STATUS_ONLINE 4       // Phone connected > 5, no device > 1
#define PROTOCOL_STATUS_CONNECTED 5    // Phone disconnected > 4, no device > 1

#define MAGIC 0x55aa55aa
#define MAGIC_ENC 0x55bb55bb

#define CMD_OPEN 1
#define CMD_PLUGGED 2
#define CMD_STATE 3
#define CMD_UNPLUGGED 4
#define CMD_TOUCH 5
#define CMD_VIDEO_DATA 6
#define CMD_AUDIO_DATA 7
#define CMD_CONTROL 8
#define CMD_UNKNOWN_9 9
#define CMD_APP_INFO 10
#define CMD_BLUETOOTH_INFO 13
#define CMD_WIFI_INFO 14
#define CMD_DEVICE_LIST 18
#define CMD_MULTI_TOUCH 23
#define CMD_JSON_CONTROL 25
#define CMD_MANUFACTURER 20
#define CMD_UNKNOWN_38 38
#define CMD_MEDIA_INFO 42
#define CMD_SEND_FILE 153
#define CMD_UNKNOWN_136 136
#define CMD_DAYNIGHT 162
#define CMD_HEARTBEAT 170
#define CMD_VERSION 204
#define CMD_ENCRYPTION 240

#define BTN_SIRI 5
#define BTN_MICROPHONE 7
#define BTN_SCREEN_REFRESH 12
// Navigation buttons
#define BTN_LEFT 100
#define BTN_RIGHT 101
#define BTN_SELECT_DOWN 104
#define BTN_SELECT_UP 105
#define BTN_BACK 106
#define BTN_DOWN 114
#define BTN_HOME 200
// Play control buttons
#define BTN_PLAY 201
#define BTN_PAUSE 202
#define BTN_203 203 // pause/resume??
#define BTN_NEXT_TRACK 204
#define BTN_PREVIOUS_TRACK 205
// Unknown buttons, volume????
#define BTN_300 300
#define BTN_301 301

// Carlinkit multitouch per-point action (CMD_MULTI_TOUCH). Distinct from the
// single-touch action codes (14/15/16); the AA backend maps these to its own
// PointerAction enum.
#define MT_ACTION_UP 0
#define MT_ACTION_DOWN 1
#define MT_ACTION_MOVE 2

#define AUDIO_BUFFER_SIZE 2560
#define AUDIO_BUFFER_OFFSET 12

#define OFFSET_AUDIO_FORMAT 0

struct ProtocolCmdEntry
{
    int cmd;
    const char *name;
};

const ProtocolCmdEntry protocolCmdList[] = {
    {CMD_OPEN, "Open"},
    {CMD_PLUGGED, "Plugged"},
    {CMD_STATE, "State"},
    {CMD_UNPLUGGED, "Unplugged"},
    {CMD_TOUCH, "Touch"},
    {CMD_VIDEO_DATA, "Video"},
    {CMD_AUDIO_DATA, "Audio"},
    {CMD_CONTROL, "Control Bin"},
    {CMD_APP_INFO, "AppInfo"},
    {CMD_BLUETOOTH_INFO, "Bluetooth Info"},
    {CMD_WIFI_INFO, "WiFi Info"},
    {CMD_DEVICE_LIST, "Device List"},
    {CMD_MULTI_TOUCH, "Multi Touch"},
    {CMD_JSON_CONTROL, "Control JSON"},
    {CMD_MANUFACTURER, "Manufacturer"},
    {CMD_MEDIA_INFO, "Media info"},
    {CMD_SEND_FILE, "File"},
    {CMD_DAYNIGHT, "DeyNight Mode"},
    {CMD_HEARTBEAT, "Heartbeat"},
    {CMD_VERSION, "Version"},
    {CMD_ENCRYPTION, "Encryption"}};


#endif /* SRC_PROTOCOL_PROTOCOL_CONST */
