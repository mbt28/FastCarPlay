#ifndef SRC_PROTOCOL_AA_AA_PROTO
#define SRC_PROTOCOL_AA_AA_PROTO

#include <cstdint>
#include <string>
#include <vector>

// Serialize / parse the protobuf bodies of the Android Auto messages the
// head unit exchanges (nanopb over the vendored aap_protobuf definitions).
// Builders return the encoded body only -- the u16 message id and framing
// are added by AaConnection. Empty vector = encode failure.
namespace aa_proto
{
using Bytes = std::vector<uint8_t>;

// control channel
Bytes serviceDiscoveryResponse();
Bytes channelOpenResponse(int32_t status = 0);
Bytes authComplete(int32_t status = 0);
Bytes pingRequest(int64_t timestamp);
Bytes pingResponse(int64_t timestamp);
Bytes audioFocusNotification(int requestType);
Bytes navFocusNotification();
Bytes byeByeResponse();

// AV channels
Bytes mediaSetupResponse(uint32_t maxUnacked);
Bytes mediaAck(int32_t sessionId);
Bytes videoFocusNotification(bool focused, bool unsolicited);

// input channel
Bytes keyBindingResponse(int32_t status = 0);
Bytes inputReportTouch(uint64_t timestamp, uint32_t x, uint32_t y, int action);
Bytes inputReportKey(uint64_t timestamp, uint32_t keycode, bool down);

// sensor channel
Bytes sensorResponse(int32_t status = 0);
Bytes sensorBatchDrivingStatus(int32_t status);
Bytes sensorBatchNightMode(bool night);

// parsers (return false on decode failure)
bool parseChannelOpenRequest(const uint8_t *data, size_t length, int32_t &serviceId);
bool parseServiceDiscoveryRequest(const uint8_t *data, size_t length, std::string &deviceName);
bool parseMediaStart(const uint8_t *data, size_t length, int32_t &sessionId);
bool parseSensorRequest(const uint8_t *data, size_t length, int32_t &sensorType);
bool parseAudioFocusRequest(const uint8_t *data, size_t length, int32_t &requestType);
bool parsePingRequest(const uint8_t *data, size_t length, int64_t &timestamp);

} // namespace aa_proto

#endif /* SRC_PROTOCOL_AA_AA_PROTO */
