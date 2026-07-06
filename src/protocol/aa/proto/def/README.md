# Android Auto protocol definitions

`aap_protobuf/` holds the `.proto` sources for the native wired Android Auto
backend, taken from [LIVI](https://github.com/f-io/LIVI) (GPL-3.0,
`src/main/services/projection/driver/aa/protos/aap_protobuf`), which in turn
derive from the aasdk / OpenAuto reverse-engineered GAL protocol. Only the
subset the head unit actually uses is vendored; `service/Service.proto` is
additionally trimmed (see its header comment) so the radio / media-browser /
bluetooth / wifi-projection subtrees aren't pulled in. Field numbers are
unchanged.

`aap.options` sets the static allocation sizes for
[nanopb](https://github.com/nanopb/nanopb) (the generated C sources live in
`../aap_protobuf/`, the nanopb 0.4.9.1 runtime in `../../nanopb/`).

## Regenerating

Only needed when a `.proto` or `aap.options` changes:

```sh
python3 -m venv /tmp/npb && /tmp/npb/bin/pip install nanopb==0.4.9.1 grpcio-tools
cd src/protocol/aa/proto/def
/tmp/npb/bin/nanopb_generator -I . --output-dir=.. -f aap.options \
    $(find aap_protobuf -name '*.proto')
```

Then run the round-trip self-test:

```sh
cd examples && make aa_proto_test && ../out/aa_proto_test
```
