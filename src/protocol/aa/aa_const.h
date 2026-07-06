#ifndef SRC_PROTOCOL_AA_AA_CONST
#define SRC_PROTOCOL_AA_AA_CONST

// Android Auto (GAL) wire-protocol constants for the native wired backend.
// Message ids and enum values mirror the generated protobuf enums in
// proto/aap_protobuf (ControlMessageType, MediaMessageId, InputMessageId,
// SensorMessageId); the short names here keep the protocol code readable.

// --- AOAP (accessory-mode switch) ---
#define AOAP_VID_GOOGLE 0x18D1
#define AOAP_PID_ACCESSORY 0x2D00
#define AOAP_PID_ACCESSORY_ADB 0x2D01
#define AOAP_REQ_GET_PROTOCOL 51
#define AOAP_REQ_SEND_STRING 52
#define AOAP_REQ_START 53
#define AOAP_STR_MANUFACTURER 0
#define AOAP_STR_MODEL 1
#define AOAP_STR_DESCRIPTION 2
#define AOAP_STR_VERSION 3
#define AOAP_STR_URI 4
#define AOAP_STR_SERIAL 5
// The manufacturer/model pair is what makes the phone start Android Auto.
#define AOAP_MANUFACTURER "Android"
#define AOAP_MODEL "Android Auto"
#define AOAP_DESCRIPTION "Android Auto"
#define AOAP_VERSION "1.0"
#define AOAP_URI "https://github.com/mbt28/FastCarPlay"
#define AOAP_SERIAL "FastCarPlay"

// --- Reconnect cadence (mirrors connection.h) ---
#define AA_RECONNECT_TIMEOUT 200
#define AA_CONNECT_RETRY 20
#define AA_HEARTBEAT_DELAY 3000

// --- Frame header flags (byte 1) ---
#define AA_FRAME_FIRST 0x01
#define AA_FRAME_LAST 0x02
#define AA_FRAME_CONTROL 0x04
#define AA_FRAME_ENCRYPTED 0x08
#define AA_FRAME_BULK (AA_FRAME_FIRST | AA_FRAME_LAST)

// Frame flag classes chosen per message, mirroring LIVI. PLAINTEXT is used
// for version/SSL-handshake/auth-complete/ping even after TLS is up;
// ENC_CONTROL only for channel-open responses; ENC_SIGNAL for every other
// encrypted message. The FIRST/LAST bits are added per fragment on top.
#define AA_FLAG_PLAINTEXT 0   // 0x03 base (FIRST|LAST), not encrypted
#define AA_FLAG_ENC_SIGNAL 1  // 0x0b base (FIRST|LAST|ENCRYPTED)
#define AA_FLAG_ENC_CONTROL 2 // 0x0f base (FIRST|LAST|CONTROL|ENCRYPTED)

// Maximum payload carried by a single frame (sending side; the receive path
// only enforces the reassembled-message cap below).
#define AA_MAX_FRAME_PAYLOAD 0x4000
// Reassembled messages are capped like Carlinkit messages.
#define AA_MAX_MESSAGE_SIZE (2 * 1024 * 1024)

// --- Channel ids (assigned by our service discovery response) ---
#define AA_CH_CONTROL 0
#define AA_CH_SENSOR 1
#define AA_CH_VIDEO 3
#define AA_CH_MEDIA_AUDIO 4
#define AA_CH_SPEECH_AUDIO 5
#define AA_CH_SYSTEM_AUDIO 6
#define AA_CH_INPUT 8
#define AA_CH_MIC 9
#define AA_CH_COUNT 16

// --- Version handshake (raw, not protobuf) ---
#define AA_VERSION_MAJOR 1
#define AA_VERSION_MINOR 7
#define AA_VERSION_STATUS_MATCH 0

// --- Control channel message ids (ControlMessageType) ---
#define AA_MSG_VERSION_REQUEST 0x0001
#define AA_MSG_VERSION_RESPONSE 0x0002
#define AA_MSG_SSL_HANDSHAKE 0x0003
#define AA_MSG_AUTH_COMPLETE 0x0004
#define AA_MSG_SERVICE_DISCOVERY_REQUEST 0x0005
#define AA_MSG_SERVICE_DISCOVERY_RESPONSE 0x0006
#define AA_MSG_CHANNEL_OPEN_REQUEST 0x0007
#define AA_MSG_CHANNEL_OPEN_RESPONSE 0x0008
#define AA_MSG_CHANNEL_CLOSE_NOTIFICATION 0x0009
#define AA_MSG_PING_REQUEST 0x000b
#define AA_MSG_PING_RESPONSE 0x000c
#define AA_MSG_NAV_FOCUS_REQUEST 0x000d
#define AA_MSG_NAV_FOCUS_NOTIFICATION 0x000e
#define AA_MSG_BYEBYE_REQUEST 0x000f
#define AA_MSG_BYEBYE_RESPONSE 0x0010
#define AA_MSG_VOICE_SESSION_NOTIFICATION 0x0011
#define AA_MSG_AUDIO_FOCUS_REQUEST 0x0012
#define AA_MSG_AUDIO_FOCUS_NOTIFICATION 0x0013

// --- AV channel message ids (MediaMessageId) ---
#define AA_MSG_MEDIA_DATA 0x0000         // u64BE timestamp + payload
#define AA_MSG_MEDIA_CODEC_CONFIG 0x0001 // payload only (H.264 SPS/PPS)
#define AA_MSG_MEDIA_SETUP 0x8000
#define AA_MSG_MEDIA_START 0x8001
#define AA_MSG_MEDIA_STOP 0x8002
#define AA_MSG_MEDIA_CONFIG 0x8003
#define AA_MSG_MEDIA_ACK 0x8004
#define AA_MSG_VIDEO_FOCUS_REQUEST 0x8007
#define AA_MSG_VIDEO_FOCUS_NOTIFICATION 0x8008

// --- Input channel message ids (InputMessageId) ---
#define AA_MSG_INPUT_REPORT 0x8001
#define AA_MSG_KEY_BINDING_REQUEST 0x8002
#define AA_MSG_KEY_BINDING_RESPONSE 0x8003

// --- Sensor channel message ids (SensorMessageId) ---
#define AA_MSG_SENSOR_REQUEST 0x8001
#define AA_MSG_SENSOR_RESPONSE 0x8002
#define AA_MSG_SENSOR_BATCH 0x8003

// --- Android keycodes used by the Carlinkit BTN -> AA translation ---
#define AA_KEY_HOME 3
#define AA_KEY_BACK 4
#define AA_KEY_DPAD_UP 19
#define AA_KEY_DPAD_DOWN 20
#define AA_KEY_DPAD_LEFT 21
#define AA_KEY_DPAD_RIGHT 22
#define AA_KEY_DPAD_CENTER 23
#define AA_KEY_SEARCH 84
#define AA_KEY_PLAY_PAUSE 85
#define AA_KEY_MEDIA_NEXT 87
#define AA_KEY_MEDIA_PREVIOUS 88
#define AA_KEY_MEDIA_PLAY 126
#define AA_KEY_MEDIA_PAUSE 127

// Internal Message type used on IConnection::writeQueue for AA frames the
// protocol itself generates (acks, pongs, responses). Payload layout:
// [0] channel, [1] 1 = pre-auth plaintext, [2..3] messageId u16BE, [4..] body.
#define CMD_AA_FRAME 0xAAF0
#define AA_FRAME_HEAD 4

#endif /* SRC_PROTOCOL_AA_AA_CONST */
