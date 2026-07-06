// Dump the exact ServiceDiscoveryResponse bytes aa_proto produces, so they can
// be validated against the schema with a strict (protobuf-java-equivalent)
// parser. Writes raw bytes to argv[1].  Build from examples/: make aa_sdr_dump
#include <cstdio>
#include "protocol/aa/aa_proto.h"

int main(int argc, char **argv)
{
    aa_proto::Bytes sdr = aa_proto::serviceDiscoveryResponse();
    fprintf(stderr, "SDR %zu bytes\n", sdr.size());
    if (argc > 1)
    {
        FILE *f = fopen(argv[1], "wb");
        if (f)
        {
            fwrite(sdr.data(), 1, sdr.size(), f);
            fclose(f);
        }
    }
    return sdr.empty() ? 1 : 0;
}
