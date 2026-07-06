// Round-trip check for aa_proto::inputReportMultiTouch: build a 2-pointer touch
// report, decode it, and confirm the pointers, action and action_index survive.
// Build from examples/: make aa_mt_test && ../out/aa_mt_test
#include <cstdio>
#include <pb_decode.h>

#include "protocol/aa/aa_proto.h"
#include "aap_protobuf/service/inputsource/message/InputReport.pb.h"

int main()
{
    aa_proto::TouchPoint pts[2] = {{100, 200, 0}, {700, 400, 1}};
    // second finger just went down -> POINTER_DOWN(5) at index 1
    aa_proto::Bytes buf = aa_proto::inputReportMultiTouch(123456789ULL, pts, 2, 5, 1);
    printf("InputReport (multitouch) %zu bytes\n", buf.size());
    if (buf.empty()) { fprintf(stderr, "FAIL: encode produced nothing\n"); return 1; }

    aap_protobuf_service_inputsource_message_InputReport rep =
        aap_protobuf_service_inputsource_message_InputReport_init_zero;
    pb_istream_t in = pb_istream_from_buffer(buf.data(), buf.size());
    if (!pb_decode(&in, aap_protobuf_service_inputsource_message_InputReport_fields, &rep)) {
        fprintf(stderr, "FAIL: decode: %s\n", in.errmsg ? in.errmsg : "?");
        return 1;
    }

    int rc = 0;
    if (!rep.has_touch_event) { fprintf(stderr, "FAIL: no touch_event\n"); rc = 1; }
    if (rep.touch_event.pointer_data_count != 2) { fprintf(stderr, "FAIL: count=%d\n", (int)rep.touch_event.pointer_data_count); rc = 1; }
    if (rep.touch_event.pointer_data[0].x != 100 || rep.touch_event.pointer_data[0].y != 200) { fprintf(stderr, "FAIL: p0\n"); rc = 1; }
    if (rep.touch_event.pointer_data[1].x != 700 || rep.touch_event.pointer_data[1].pointer_id != 1) { fprintf(stderr, "FAIL: p1\n"); rc = 1; }
    if (rep.touch_event.action != aap_protobuf_service_inputsource_message_PointerAction_ACTION_POINTER_DOWN) { fprintf(stderr, "FAIL: action=%d\n", (int)rep.touch_event.action); rc = 1; }
    if (rep.touch_event.action_index != 1) { fprintf(stderr, "FAIL: action_index=%d\n", (int)rep.touch_event.action_index); rc = 1; }

    printf(rc ? "aa_mt_test: FAILED\n" : "aa_mt_test: all checks passed\n");
    return rc;
}
