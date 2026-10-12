#ifndef SRC_PROTOCOL_PROTOCOL_CONST
#define SRC_PROTOCOL_PROTOCOL_CONST

#include <cstdint>

#define PROTOCOL_STATUS_UNKNOWN -1     // Manual > 0
#define PROTOCOL_STATUS_INITIALISING 0 // Initialised > 1
#define PROTOCOL_STATUS_NO_DEVICE 1    // Start linking > 3
#define PROTOCOL_STATUS_LINKING 2      // Linked > 4, Failed in sequence > 3
#define PROTOCOL_STATUS_ERROR 3        // Linked > 4, no device in sequence > 1
#define PROTOCOL_STATUS_ONLINE 4       // Phone connected > 5, no device > 1
#define PROTOCOL_STATUS_CONNECTED 5    // Phone disconnected > 4, no device > 1

// Internal Message types for the app's media/input queues (historically the
// carlinkit wire command ids; now just labels the backends agree on).
#define CMD_TOUCH 5
#define CMD_VIDEO_DATA 6
#define CMD_AUDIO_DATA 7
#define CMD_CONTROL 8
#define CMD_MULTI_TOUCH 23
#define CMD_HEARTBEAT 170

// App button codes (CMD_CONTROL payload), shared by the input paths and the
// Android Auto key translation.
#define BTN_SIRI 5
#define BTN_SCREEN_REFRESH 12
// Navigation buttons
#define BTN_LEFT 100
#define BTN_RIGHT 101
#define BTN_SELECT_DOWN 104
#define BTN_SELECT_UP 105
#define BTN_BACK 106
#define BTN_UP 113
#define BTN_DOWN 114
#define BTN_HOME 200
// Play control buttons
#define BTN_PLAY 201
#define BTN_PAUSE 202
#define BTN_203 203 // pause/resume??
#define BTN_NEXT_TRACK 204
#define BTN_PREVIOUS_TRACK 205
// Call accept/reject (desktop key mappings; die with the SDL path)
#define BTN_300 300
#define BTN_301 301

// Multitouch per-point action (CMD_MULTI_TOUCH). Distinct from the
// single-touch action codes (14/15/16); the AA backend maps these to its own
// PointerAction enum.
#define MT_ACTION_UP 0
#define MT_ACTION_DOWN 1
#define MT_ACTION_MOVE 2

#define AUDIO_BUFFER_OFFSET 12

#define OFFSET_AUDIO_FORMAT 0

#endif /* SRC_PROTOCOL_PROTOCOL_CONST */
