#ifndef SRC_DECODER
#define SRC_DECODER

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
}

#include <atomic>
#include <thread>

#include "idecoder.h"
#include "struct/atomic_queue.h"
#include "protocol/message.h"

class Decoder : public IDecoder
{
public:
    Decoder();
    ~Decoder();

    void start(AtomicQueue<Message> *data, AVCodecID codecId) override;
    void stop() override;
    void flush() override;

private:
    void runner();
    void loop(AVCodecContext *context, AVCodecParserContext *parser, AVPacket *packet, AVFrame *frame);
    static AVCodecContext *load_codec(AVCodecID codec_id);

    std::thread _thread;
    AVCodecContext* _context;
    AVCodecID _codecId;
    std::atomic<bool> _active;
    AtomicQueue<Message> *_data;
};

#endif /* SRC_DECODER */
