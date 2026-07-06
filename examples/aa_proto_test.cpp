// Round-trip self-test for the vendored nanopb + generated AA protobuf code.
// Encodes a representative ServiceDiscoveryResponse (the most deeply nested
// message the head unit sends) plus a touch InputReport, decodes them back
// and checks the fields survived. Exits 0 on success.
//
// Build from examples/: make aa_proto_test && ../out/aa_proto_test

#include <cstdio>
#include <cstring>

#include <pb_encode.h>
#include <pb_decode.h>

#include "aap_protobuf/service/control/message/ServiceDiscoveryResponse.pb.h"
#include "aap_protobuf/service/inputsource/message/InputReport.pb.h"

#define CHECK(cond)                                                  \
    do                                                               \
    {                                                                \
        if (!(cond))                                                 \
        {                                                            \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            return 1;                                                \
        }                                                            \
    } while (0)

int main()
{
    // --- ServiceDiscoveryResponse: control + video + media audio + input ---
    aap_protobuf_service_control_message_ServiceDiscoveryResponse sdr =
        aap_protobuf_service_control_message_ServiceDiscoveryResponse_init_zero;

    sdr.channels_count = 3;

    // video sink (channel id 3)
    auto &video = sdr.channels[0];
    video.id = 3;
    video.has_media_sink_service = true;
    video.media_sink_service.has_available_type = true;
    video.media_sink_service.available_type =
        aap_protobuf_service_media_shared_message_MediaCodecType_MEDIA_CODEC_VIDEO_H264_BP;
    video.media_sink_service.video_configs_count = 1;
    auto &vc = video.media_sink_service.video_configs[0];
    vc.has_codec_resolution = true;
    vc.codec_resolution =
        aap_protobuf_service_media_sink_message_VideoCodecResolutionType_VIDEO_800x480;
    vc.has_frame_rate = true;
    vc.frame_rate = aap_protobuf_service_media_sink_message_VideoFrameRateType_VIDEO_FPS_30;
    vc.has_density = true;
    vc.density = 120;

    // media audio sink (channel id 4)
    auto &audio = sdr.channels[1];
    audio.id = 4;
    audio.has_media_sink_service = true;
    audio.media_sink_service.has_available_type = true;
    audio.media_sink_service.available_type =
        aap_protobuf_service_media_shared_message_MediaCodecType_MEDIA_CODEC_AUDIO_PCM;
    audio.media_sink_service.audio_configs_count = 1;
    auto &ac = audio.media_sink_service.audio_configs[0];
    ac.sampling_rate = 48000;
    ac.number_of_channels = 2;
    ac.number_of_bits = 16;

    // input source (channel id 8)
    auto &input = sdr.channels[2];
    input.id = 8;
    input.has_input_source_service = true;
    input.input_source_service.touchscreen_count = 1;
    input.input_source_service.touchscreen[0].width = 800;
    input.input_source_service.touchscreen[0].height = 480;

    sdr.has_headunit_info = true;
    strcpy(sdr.headunit_info.make, "FastCarPlay");
    sdr.headunit_info.has_make = true;

    uint8_t buffer[1024];
    pb_ostream_t out = pb_ostream_from_buffer(buffer, sizeof(buffer));
    CHECK(pb_encode(&out, aap_protobuf_service_control_message_ServiceDiscoveryResponse_fields, &sdr));
    printf("ServiceDiscoveryResponse encoded: %u bytes\n", (unsigned)out.bytes_written);
    CHECK(out.bytes_written > 20);

    aap_protobuf_service_control_message_ServiceDiscoveryResponse back =
        aap_protobuf_service_control_message_ServiceDiscoveryResponse_init_zero;
    pb_istream_t in = pb_istream_from_buffer(buffer, out.bytes_written);
    CHECK(pb_decode(&in, aap_protobuf_service_control_message_ServiceDiscoveryResponse_fields, &back));

    CHECK(back.channels_count == 3);
    CHECK(back.channels[0].id == 3);
    CHECK(back.channels[0].media_sink_service.video_configs[0].codec_resolution ==
          aap_protobuf_service_media_sink_message_VideoCodecResolutionType_VIDEO_800x480);
    CHECK(back.channels[1].media_sink_service.audio_configs[0].sampling_rate == 48000);
    CHECK(back.channels[2].input_source_service.touchscreen[0].width == 800);
    CHECK(back.has_headunit_info && strcmp(back.headunit_info.make, "FastCarPlay") == 0);

    // --- InputReport with a touch event ---
    aap_protobuf_service_inputsource_message_InputReport report =
        aap_protobuf_service_inputsource_message_InputReport_init_zero;
    report.timestamp = 1234567890123ULL;
    report.has_touch_event = true;
    report.touch_event.pointer_data_count = 1;
    report.touch_event.pointer_data[0].x = 400;
    report.touch_event.pointer_data[0].y = 240;
    report.touch_event.pointer_data[0].pointer_id = 0;
    report.touch_event.has_action = true;
    report.touch_event.action =
        aap_protobuf_service_inputsource_message_PointerAction_ACTION_DOWN;

    pb_ostream_t out2 = pb_ostream_from_buffer(buffer, sizeof(buffer));
    CHECK(pb_encode(&out2, aap_protobuf_service_inputsource_message_InputReport_fields, &report));
    printf("InputReport encoded: %u bytes\n", (unsigned)out2.bytes_written);

    aap_protobuf_service_inputsource_message_InputReport rback =
        aap_protobuf_service_inputsource_message_InputReport_init_zero;
    pb_istream_t in2 = pb_istream_from_buffer(buffer, out2.bytes_written);
    CHECK(pb_decode(&in2, aap_protobuf_service_inputsource_message_InputReport_fields, &rback));
    CHECK(rback.touch_event.pointer_data[0].x == 400);
    CHECK(rback.touch_event.action ==
          aap_protobuf_service_inputsource_message_PointerAction_ACTION_DOWN);

    printf("aa_proto_test: all checks passed\n");
    return 0;
}
