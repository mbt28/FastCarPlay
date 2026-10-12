#ifndef SRC_PROTOCOL_MESSAGE
#define SRC_PROTOCOL_MESSAGE

#include <algorithm>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <memory>

#include "protocol/protocol_const.h"
#include "struct/multitouch.h"

// The app's internal media/input container: a type + length header and a heap
// payload. Originally the carlinkit dongle's wire format; the native backends
// kept the shape as their common currency for the video/audio/input queues.
#pragma pack(push, 1)
struct Header
{
    uint32_t magic; // unused legacy field, always 0
    int32_t length;
    uint32_t type;
    uint32_t typecheck;
};
#pragma pack(pop)

class Message
{
public:
    Message(uint32_t cmd, int32_t size = 0, uint8_t *buffer = nullptr)
        : _header({0, size, cmd, ~cmd}),
          _data(nullptr),
          _offset(0),
          _size(0)
    {
        if (size <= 0)
            return;

        if (!buffer)
        {
            allocate();
            return;
        }

        _data = buffer;
        _size = size;
    }

    Message(uint32_t cmd, uint32_t value)
        : Message(cmd, 4)
    {
        write_uint32_le(_data, value);
    }

    Message(const Message &) = delete;
    Message &operator=(const Message &) = delete;

    ~Message()
    {
        if (_data)
        {
            free(_data);
            _data = nullptr;
        }
    }

    uint8_t *allocate(uint32_t padding = 0)
    {
        if (_data != nullptr || _header.length <= 0)
            return nullptr;

        _size = _header.length + padding;
        _data = static_cast<uint8_t *>(malloc(_size));
        if (!_data)
            _size = 0;
        else
            std::fill(_data + _header.length, _data + _header.length + padding, 0);
        return _data;
    }

    int getInt(uint32_t offset) const
    {
        int result = 0;
        if (_data && offset + sizeof(int) <= _size)
            memcpy(&result, _data + offset, sizeof(int));
        return result;
    }

    bool setOffset(uint32_t offset)
    {
        if (offset >= _size)
            return false;
        _offset = offset;
        return true;
    }

    bool isMotion() const
    {
        return _header.type == CMD_TOUCH && getInt(0) == 15;
    }

    static std::unique_ptr<Message> Control(uint32_t value)
    {
        return std::unique_ptr<Message>(new Message(CMD_CONTROL, value));
    }

    static std::unique_ptr<Message> Touch(uint32_t action, float x, float y)
    {
        std::unique_ptr<Message> result(new Message(CMD_TOUCH, 16));
        write_uint32_le(result->_data, action);
        write_uint32_le(result->_data + 4, int(10000 * x));
        write_uint32_le(result->_data + 8, int(10000 * y));
        write_uint32_le(result->_data + 12, 0);
        return result;
    }

    static std::unique_ptr<Message> Click(float x, float y, bool down)
    {
        return Touch(down ? 14 : 16, x, y);
    }

    static std::unique_ptr<Message> Move(float x, float y)
    {
        return Touch(15, x, y);
    }

    static std::unique_ptr<Message> MultiTouch(const Multitouch &touches)
    {
        int count = touches.size();
        if (count == 0)
            return nullptr;

        std::unique_ptr<Message> result(new Message(CMD_MULTI_TOUCH, sizeof(Multitouch::Touch) * count));
        uint8_t *buf = result->_data;
        for (int i = 0; i < count; ++i)
        {
            const Multitouch::Touch &t = touches[i];
            write_float_le(buf + 0, t.x);
            write_float_le(buf + 4, t.y);
            write_uint32_le(buf + 8, static_cast<uint32_t>(t.state));
            write_uint32_le(buf + 12, static_cast<uint32_t>(t.id));
            buf += 16;
        }
        return result;
    }

    static std::unique_ptr<Message> HeartBeat()
    {
        return std::unique_ptr<Message>(new Message(CMD_HEARTBEAT));
    }

    // Allocate-then-fill: a `cmd` message with an uninitialised `length`-byte
    // payload (plus `padding` zeroed spare bytes) the caller writes through
    // data(). Lets a protocol backend deposit reassembled stream data straight
    // into the final buffer instead of copying through an intermediate one.
    // Check data() != nullptr before filling (allocation can fail).
    static std::unique_ptr<Message> Payload(uint32_t cmd, int32_t length, uint32_t padding = 0)
    {
        std::unique_ptr<Message> result(new Message(cmd, 0));
        result->_header.length = length;
        result->allocate(padding);
        return result;
    }

    bool allocated() const { return _header.length <= 0 || static_cast<uint32_t>(_header.length) <= _size; }
    uint32_t type() const { return _header.type; }
    int32_t length() const { return _header.length - _offset; }
    uint8_t *data() const { return _data ? _data + _offset : nullptr; }

private:
    static inline void write_uint32_le(uint8_t *dst, uint32_t value)
    {
        dst[0] = value & 0xFF;
        dst[1] = (value >> 8) & 0xFF;
        dst[2] = (value >> 16) & 0xFF;
        dst[3] = (value >> 24) & 0xFF;
    }

    static inline void write_float_le(uint8_t *dst, float value)
    {
        uint32_t bits;
        std::memcpy(&bits, &value, sizeof(bits));
        write_uint32_le(dst, bits);
    }

    Header _header;
    uint8_t *_data;
    uint32_t _offset;
    uint32_t _size;
};

#endif /* SRC_PROTOCOL_MESSAGE */
