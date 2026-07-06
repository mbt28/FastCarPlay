#include "aa_proto.h"

#include <cstring>

#include <pb_encode.h>
#include <pb_decode.h>

#include "aap_protobuf/service/control/message/ServiceDiscoveryRequest.pb.h"
#include "aap_protobuf/service/control/message/ServiceDiscoveryResponse.pb.h"
#include "aap_protobuf/service/control/message/ChannelOpenRequest.pb.h"
#include "aap_protobuf/service/control/message/ChannelOpenResponse.pb.h"
#include "aap_protobuf/service/control/message/AuthResponse.pb.h"
#include "aap_protobuf/service/control/message/PingRequest.pb.h"
#include "aap_protobuf/service/control/message/PingResponse.pb.h"
#include "aap_protobuf/service/control/message/AudioFocusRequest.pb.h"
#include "aap_protobuf/service/control/message/AudioFocusNotification.pb.h"
#include "aap_protobuf/service/control/message/NavFocusNotification.pb.h"
#include "aap_protobuf/service/control/message/ByeByeResponse.pb.h"
#include "aap_protobuf/service/media/shared/message/Config.pb.h"
#include "aap_protobuf/service/media/shared/message/Start.pb.h"
#include "aap_protobuf/service/media/source/message/Ack.pb.h"
#include "aap_protobuf/service/media/video/message/VideoFocusNotification.pb.h"
#include "aap_protobuf/service/media/sink/message/KeyBindingResponse.pb.h"
#include "aap_protobuf/service/inputsource/message/InputReport.pb.h"
#include "aap_protobuf/service/sensorsource/message/SensorRequest.pb.h"
#include "aap_protobuf/service/sensorsource/message/SensorStartResponseMessage.pb.h"
#include "aap_protobuf/service/sensorsource/message/SensorBatch.pb.h"

#include "protocol/aa/aa_const.h"
#include "struct/multitouch.h"
#include "common/logger.h"
#include "settings.h"

// The generated identifiers are fully qualified and very long; alias the
// namespaced prefixes we use repeatedly.
#define NS_CTRL(name) aap_protobuf_service_control_message_##name
#define NS_MEDIA(name) aap_protobuf_service_media_shared_message_##name
#define NS_SINK(name) aap_protobuf_service_media_sink_##name
#define NS_SINKMSG(name) aap_protobuf_service_media_sink_message_##name
#define NS_SRCMSG(name) aap_protobuf_service_media_source_message_##name
#define NS_VIDEO(name) aap_protobuf_service_media_video_message_##name
#define NS_INPUT(name) aap_protobuf_service_inputsource_##name
#define NS_INPUTMSG(name) aap_protobuf_service_inputsource_message_##name
#define NS_SENSOR(name) aap_protobuf_service_sensorsource_##name
#define NS_SENSORMSG(name) aap_protobuf_service_sensorsource_message_##name

namespace aa_proto
{

template <typename T>
static Bytes encode(const pb_msgdesc_t *fields, const T &message, size_t sizeHint = 256)
{
    Bytes out(sizeHint);
    pb_ostream_t stream = pb_ostream_from_buffer(out.data(), out.size());
    if (!pb_encode(&stream, fields, &message))
    {
        // Retry once with an exact size in case the hint was too small.
        size_t needed = 0;
        if (!pb_get_encoded_size(&needed, fields, &message))
        {
            log_w("AA protobuf encode failed > %s", stream.errmsg ? stream.errmsg : "unknown");
            return Bytes();
        }
        out.resize(needed);
        stream = pb_ostream_from_buffer(out.data(), out.size());
        if (!pb_encode(&stream, fields, &message))
        {
            log_w("AA protobuf encode failed > %s", stream.errmsg ? stream.errmsg : "unknown");
            return Bytes();
        }
    }
    out.resize(stream.bytes_written);
    return out;
}

template <typename T>
static bool decode(const pb_msgdesc_t *fields, T &message, const uint8_t *data, size_t length)
{
    pb_istream_t stream = pb_istream_from_buffer(data, length);
    if (!pb_decode(&stream, fields, &message))
    {
        log_w("AA protobuf decode failed > %s", stream.errmsg ? stream.errmsg : "unknown");
        return false;
    }
    return true;
}

static void setString(char *dst, size_t cap, const char *src, bool &has)
{
    snprintf(dst, cap, "%s", src);
    has = true;
}

// Video/touch dimensions for the advertised resolution setting (1/2/3).
static void videoDimensions(int &width, int &height)
{
    switch (Settings::aaResolution)
    {
    case 3:
        width = 1920;
        height = 1080;
        break;
    case 2:
        width = 1280;
        height = 720;
        break;
    default:
        width = 800;
        height = 480;
        break;
    }
}

Bytes serviceDiscoveryResponse()
{
    NS_CTRL(ServiceDiscoveryResponse) sdr = {};

    int index = 0;

    // Sensor source (channel 1): driving status is mandatory for projection,
    // night mode drives the day/night theme.
    {
        auto &ch = sdr.channels[index++];
        ch.id = AA_CH_SENSOR;
        ch.has_sensor_source_service = true;
        ch.sensor_source_service.sensors_count = 2;
        ch.sensor_source_service.sensors[0].sensor_type =
            NS_SENSORMSG(SensorType_SENSOR_DRIVING_STATUS_DATA);
        ch.sensor_source_service.sensors[1].sensor_type =
            NS_SENSORMSG(SensorType_SENSOR_NIGHT_MODE);
    }

    // Video sink (channel 3)
    {
        int width, height;
        videoDimensions(width, height);
        (void)width;
        (void)height;
        auto &ch = sdr.channels[index++];
        ch.id = AA_CH_VIDEO;
        ch.has_media_sink_service = true;
        auto &sink = ch.media_sink_service;
        sink.has_available_type = true;
        sink.available_type = NS_MEDIA(MediaCodecType_MEDIA_CODEC_VIDEO_H264_BP);
        sink.has_available_while_in_call = true;
        sink.available_while_in_call = true;
        sink.has_display_id = true;
        sink.display_id = 0;
        sink.video_configs_count = 1;
        auto &config = sink.video_configs[0];
        config.has_codec_resolution = true;
        config.codec_resolution =
            (NS_SINKMSG(VideoCodecResolutionType))(Settings::aaResolution.value);
        config.has_frame_rate = true;
        config.frame_rate = Settings::aaFps == 60 ? NS_SINKMSG(VideoFrameRateType_VIDEO_FPS_60)
                                                  : NS_SINKMSG(VideoFrameRateType_VIDEO_FPS_30);
        config.has_width_margin = true;
        config.width_margin = 0;
        config.has_height_margin = true;
        config.height_margin = 0;
        config.has_density = true;
        config.density = Settings::dpi;
        config.has_pixel_aspect_ratio_e4 = true;
        config.pixel_aspect_ratio_e4 = 10000;
        // The phone maps each config index to a codec via video_codec_type;
        // without it the video config is ambiguous and the phone rejects the
        // whole discovery response.
        config.has_video_codec_type = true;
        config.video_codec_type = NS_MEDIA(MediaCodecType_MEDIA_CODEC_VIDEO_H264_BP);
    }

    // Audio sinks: media 48k stereo (4), guidance (5) and system (6) 16k mono.
    struct AudioDef
    {
        int channel;
        NS_SINKMSG(AudioStreamType) type;
        uint32_t rate;
        uint32_t channels;
    };
    const AudioDef audio[] = {
        {AA_CH_MEDIA_AUDIO, NS_SINKMSG(AudioStreamType_AUDIO_STREAM_MEDIA), 48000, 2},
        {AA_CH_SPEECH_AUDIO, NS_SINKMSG(AudioStreamType_AUDIO_STREAM_GUIDANCE), 16000, 1},
        {AA_CH_SYSTEM_AUDIO, NS_SINKMSG(AudioStreamType_AUDIO_STREAM_SYSTEM_AUDIO), 16000, 1},
    };
    for (const AudioDef &def : audio)
    {
        auto &ch = sdr.channels[index++];
        ch.id = def.channel;
        ch.has_media_sink_service = true;
        auto &sink = ch.media_sink_service;
        sink.has_available_type = true;
        sink.available_type = NS_MEDIA(MediaCodecType_MEDIA_CODEC_AUDIO_PCM);
        sink.has_audio_type = true;
        sink.audio_type = def.type;
        sink.audio_configs_count = 1;
        sink.audio_configs[0].sampling_rate = def.rate;
        sink.audio_configs[0].number_of_bits = 16;
        sink.audio_configs[0].number_of_channels = def.channels;
    }

    // Microphone source (channel 9): Android Auto expects the head unit to
    // offer a microphone for the Assistant; phones reject a mic-less HU.
    {
        auto &ch = sdr.channels[index++];
        ch.id = AA_CH_MIC;
        ch.has_media_source_service = true;
        auto &source = ch.media_source_service;
        source.has_available_type = true;
        source.available_type = NS_MEDIA(MediaCodecType_MEDIA_CODEC_AUDIO_PCM);
        source.has_available_while_in_call = true;
        source.available_while_in_call = true;
        source.has_audio_config = true;
        source.audio_config.sampling_rate = 16000;
        source.audio_config.number_of_bits = 16;
        source.audio_config.number_of_channels = 1;
    }

    // Input source (channel 8): a touchscreen in the video coordinate space
    // plus the keycodes the Carlinkit keymap can deliver.
    {
        int width, height;
        videoDimensions(width, height);
        auto &ch = sdr.channels[index++];
        ch.id = AA_CH_INPUT;
        ch.has_input_source_service = true;
        auto &input = ch.input_source_service;
        input.touchscreen_count = 1;
        input.touchscreen[0].width = width;
        input.touchscreen[0].height = height;
        static const int32_t keycodes[] = {
            AA_KEY_HOME, AA_KEY_BACK, AA_KEY_DPAD_UP, AA_KEY_DPAD_DOWN, AA_KEY_DPAD_LEFT,
            AA_KEY_DPAD_RIGHT, AA_KEY_DPAD_CENTER, AA_KEY_SEARCH, AA_KEY_PLAY_PAUSE,
            AA_KEY_MEDIA_NEXT, AA_KEY_MEDIA_PREVIOUS, AA_KEY_MEDIA_PLAY, AA_KEY_MEDIA_PAUSE};
        input.keycodes_supported_count = sizeof(keycodes) / sizeof(keycodes[0]);
        for (size_t i = 0; i < input.keycodes_supported_count; i++)
            input.keycodes_supported[i] = keycodes[i];
    }

    sdr.channels_count = index;

    // Identification. The deprecated top-level strings are still what older
    // phone versions display; headunit_info is the current home.
    setString(sdr.make, sizeof(sdr.make), "FastCarPlay", sdr.has_make);
    setString(sdr.model, sizeof(sdr.model), "FastCarPlay", sdr.has_model);
    setString(sdr.year, sizeof(sdr.year), "2026", sdr.has_year);
    setString(sdr.vehicle_id, sizeof(sdr.vehicle_id), "20260001", sdr.has_vehicle_id);
    setString(sdr.display_name, sizeof(sdr.display_name), "FastCarPlay", sdr.has_display_name);
    sdr.has_driver_position = true;
    sdr.driver_position = Settings::leftDrive ? NS_CTRL(DriverPosition_DRIVER_POSITION_LEFT)
                                              : NS_CTRL(DriverPosition_DRIVER_POSITION_RIGHT);
    sdr.has_headunit_info = true;
    setString(sdr.headunit_info.make, sizeof(sdr.headunit_info.make), "FastCarPlay",
              sdr.headunit_info.has_make);
    setString(sdr.headunit_info.model, sizeof(sdr.headunit_info.model), "F1C200s",
              sdr.headunit_info.has_model);
    setString(sdr.headunit_info.head_unit_make, sizeof(sdr.headunit_info.head_unit_make),
              "FastCarPlay", sdr.headunit_info.has_head_unit_make);
    setString(sdr.headunit_info.head_unit_model, sizeof(sdr.headunit_info.head_unit_model),
              "F1C200s", sdr.headunit_info.has_head_unit_model);
    setString(sdr.headunit_info.head_unit_software_build,
              sizeof(sdr.headunit_info.head_unit_software_build), "1",
              sdr.headunit_info.has_head_unit_software_build);
    setString(sdr.headunit_info.head_unit_software_version,
              sizeof(sdr.headunit_info.head_unit_software_version), "1.0",
              sdr.headunit_info.has_head_unit_software_version);

    sdr.has_can_play_native_media_during_vr = true;
    sdr.can_play_native_media_during_vr = true;
    sdr.has_probe_for_support = true;
    sdr.probe_for_support = false;

    // Ping cadence the phone should use (matches LIVI). Without a connection
    // configuration some phones reject the discovery response outright.
    sdr.has_connection_configuration = true;
    sdr.connection_configuration.has_ping_configuration = true;
    sdr.connection_configuration.ping_configuration.has_timeout_ms = true;
    sdr.connection_configuration.ping_configuration.timeout_ms = 5000;
    sdr.connection_configuration.ping_configuration.has_interval_ms = true;
    sdr.connection_configuration.ping_configuration.interval_ms = 1500;
    sdr.connection_configuration.ping_configuration.has_high_latency_threshold_ms = true;
    sdr.connection_configuration.ping_configuration.high_latency_threshold_ms = 500;
    sdr.connection_configuration.ping_configuration.has_tracked_ping_count = true;
    sdr.connection_configuration.ping_configuration.tracked_ping_count = 5;

    return encode(NS_CTRL(ServiceDiscoveryResponse_fields), sdr, 1024);
}

Bytes channelOpenResponse(int32_t status)
{
    NS_CTRL(ChannelOpenResponse) msg = {};
    msg.status = (aap_protobuf_shared_MessageStatus)status;
    return encode(NS_CTRL(ChannelOpenResponse_fields), msg, 16);
}

Bytes authComplete(int32_t status)
{
    NS_CTRL(AuthResponse) msg = {};
    msg.status = status;
    return encode(NS_CTRL(AuthResponse_fields), msg, 16);
}

Bytes pingRequest(int64_t timestamp)
{
    NS_CTRL(PingRequest) msg = {};
    msg.timestamp = timestamp;
    return encode(NS_CTRL(PingRequest_fields), msg, 32);
}

Bytes pingResponse(int64_t timestamp)
{
    NS_CTRL(PingResponse) msg = {};
    msg.timestamp = timestamp;
    return encode(NS_CTRL(PingResponse_fields), msg, 32);
}

Bytes audioFocusNotification(int requestType)
{
    NS_CTRL(AudioFocusNotification) msg = {};
    switch (requestType)
    {
    case NS_CTRL(AudioFocusRequestType_AUDIO_FOCUS_RELEASE):
        msg.focus_state = NS_CTRL(AudioFocusStateType_AUDIO_FOCUS_STATE_LOSS);
        break;
    case NS_CTRL(AudioFocusRequestType_AUDIO_FOCUS_GAIN_TRANSIENT):
        msg.focus_state = NS_CTRL(AudioFocusStateType_AUDIO_FOCUS_STATE_GAIN_TRANSIENT);
        break;
    case NS_CTRL(AudioFocusRequestType_AUDIO_FOCUS_GAIN_TRANSIENT_MAY_DUCK):
        msg.focus_state =
            NS_CTRL(AudioFocusStateType_AUDIO_FOCUS_STATE_GAIN_TRANSIENT_GUIDANCE_ONLY);
        break;
    default:
        msg.focus_state = NS_CTRL(AudioFocusStateType_AUDIO_FOCUS_STATE_GAIN);
        break;
    }
    msg.has_unsolicited = true;
    msg.unsolicited = false;
    return encode(NS_CTRL(AudioFocusNotification_fields), msg, 16);
}

Bytes navFocusNotification()
{
    NS_CTRL(NavFocusNotification) msg = {};
    msg.focus_type = NS_CTRL(NavFocusType_NAV_FOCUS_PROJECTED);
    return encode(NS_CTRL(NavFocusNotification_fields), msg, 16);
}

Bytes byeByeResponse()
{
    NS_CTRL(ByeByeResponse) msg = {};
    return encode(NS_CTRL(ByeByeResponse_fields), msg, 8);
}

Bytes mediaSetupResponse(uint32_t maxUnacked)
{
    NS_MEDIA(Config) msg = {};
    msg.status = NS_MEDIA(Config_Status_STATUS_READY);
    msg.has_max_unacked = true;
    msg.max_unacked = maxUnacked;
    msg.configuration_indices_count = 1;
    msg.configuration_indices[0] = 0;
    return encode(NS_MEDIA(Config_fields), msg, 32);
}

Bytes mediaAck(int32_t sessionId)
{
    NS_SRCMSG(Ack) msg = {};
    msg.session_id = sessionId;
    msg.has_ack = true;
    msg.ack = 1;
    return encode(NS_SRCMSG(Ack_fields), msg, 16);
}

Bytes videoFocusNotification(bool focused, bool unsolicited)
{
    NS_VIDEO(VideoFocusNotification) msg = {};
    msg.has_focus = true;
    msg.focus = focused ? NS_VIDEO(VideoFocusMode_VIDEO_FOCUS_PROJECTED)
                        : NS_VIDEO(VideoFocusMode_VIDEO_FOCUS_NATIVE);
    msg.has_unsolicited = true;
    msg.unsolicited = unsolicited;
    return encode(NS_VIDEO(VideoFocusNotification_fields), msg, 16);
}

Bytes keyBindingResponse(int32_t status)
{
    NS_SINKMSG(KeyBindingResponse) msg = {};
    msg.status = status;
    return encode(NS_SINKMSG(KeyBindingResponse_fields), msg, 16);
}

Bytes inputReportTouch(uint64_t timestamp, uint32_t x, uint32_t y, int action)
{
    NS_INPUTMSG(InputReport) msg = {};
    msg.timestamp = timestamp;
    msg.has_touch_event = true;
    msg.touch_event.pointer_data_count = 1;
    msg.touch_event.pointer_data[0].x = x;
    msg.touch_event.pointer_data[0].y = y;
    msg.touch_event.pointer_data[0].pointer_id = 0;
    msg.touch_event.has_action = true;
    msg.touch_event.action = (NS_INPUTMSG(PointerAction))action;
    return encode(NS_INPUTMSG(InputReport_fields), msg, 48);
}

Bytes inputReportMultiTouch(uint64_t timestamp, const TouchPoint *points, int count,
                            int action, int actionIndex)
{
    NS_INPUTMSG(InputReport) msg = {};
    msg.timestamp = timestamp;
    msg.has_touch_event = true;
    if (count > MUTLITOUCH_MAX_TOUCH)
        count = MUTLITOUCH_MAX_TOUCH;
    msg.touch_event.pointer_data_count = count;
    for (int i = 0; i < count; i++)
    {
        msg.touch_event.pointer_data[i].x = points[i].x;
        msg.touch_event.pointer_data[i].y = points[i].y;
        msg.touch_event.pointer_data[i].pointer_id = points[i].id;
    }
    msg.touch_event.has_action_index = true;
    msg.touch_event.action_index = actionIndex;
    msg.touch_event.has_action = true;
    msg.touch_event.action = (NS_INPUTMSG(PointerAction))action;
    return encode(NS_INPUTMSG(InputReport_fields), msg, 96);
}

Bytes inputReportKey(uint64_t timestamp, uint32_t keycode, bool down)
{
    NS_INPUTMSG(InputReport) msg = {};
    msg.timestamp = timestamp;
    msg.has_key_event = true;
    msg.key_event.keys_count = 1;
    msg.key_event.keys[0].keycode = keycode;
    msg.key_event.keys[0].down = down;
    msg.key_event.keys[0].metastate = 0;
    return encode(NS_INPUTMSG(InputReport_fields), msg, 48);
}

Bytes sensorResponse(int32_t status)
{
    NS_SENSORMSG(SensorStartResponseMessage) msg = {};
    msg.status = (aap_protobuf_shared_MessageStatus)status;
    return encode(NS_SENSORMSG(SensorStartResponseMessage_fields), msg, 16);
}

Bytes sensorBatchDrivingStatus(int32_t status)
{
    NS_SENSORMSG(SensorBatch) msg = {};
    msg.driving_status_data_count = 1;
    msg.driving_status_data[0].status = status;
    return encode(NS_SENSORMSG(SensorBatch_fields), msg, 32);
}

Bytes sensorBatchNightMode(bool night)
{
    NS_SENSORMSG(SensorBatch) msg = {};
    msg.night_mode_data_count = 1;
    msg.night_mode_data[0].has_night_mode = true;
    msg.night_mode_data[0].night_mode = night;
    return encode(NS_SENSORMSG(SensorBatch_fields), msg, 32);
}

bool parseChannelOpenRequest(const uint8_t *data, size_t length, int32_t &serviceId)
{
    NS_CTRL(ChannelOpenRequest) msg = {};
    if (!decode(NS_CTRL(ChannelOpenRequest_fields), msg, data, length))
        return false;
    serviceId = msg.service_id;
    return true;
}

bool parseServiceDiscoveryRequest(const uint8_t *data, size_t length, std::string &deviceName)
{
    NS_CTRL(ServiceDiscoveryRequest) msg = {};
    if (!decode(NS_CTRL(ServiceDiscoveryRequest_fields), msg, data, length))
        return false;
    if (msg.has_device_name)
        deviceName = msg.device_name;
    return true;
}

bool parseMediaStart(const uint8_t *data, size_t length, int32_t &sessionId)
{
    NS_MEDIA(Start) msg = {};
    if (!decode(NS_MEDIA(Start_fields), msg, data, length))
        return false;
    sessionId = msg.session_id;
    return true;
}

bool parseSensorRequest(const uint8_t *data, size_t length, int32_t &sensorType)
{
    NS_SENSORMSG(SensorRequest) msg = {};
    if (!decode(NS_SENSORMSG(SensorRequest_fields), msg, data, length))
        return false;
    sensorType = msg.type;
    return true;
}

bool parseAudioFocusRequest(const uint8_t *data, size_t length, int32_t &requestType)
{
    NS_CTRL(AudioFocusRequest) msg = {};
    if (!decode(NS_CTRL(AudioFocusRequest_fields), msg, data, length))
        return false;
    requestType = msg.audio_focus_type;
    return true;
}

bool parsePingRequest(const uint8_t *data, size_t length, int64_t &timestamp)
{
    NS_CTRL(PingRequest) msg = {};
    if (!decode(NS_CTRL(PingRequest_fields), msg, data, length))
        return false;
    timestamp = msg.timestamp;
    return true;
}

} // namespace aa_proto
