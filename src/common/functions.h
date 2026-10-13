#ifndef SRC_COMMON_FUNCTIONS
#define SRC_COMMON_FUNCTIONS

#include <cstdlib>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>

#include "common/threading.h"

extern "C"
{
#include <libavutil/error.h>
}

inline void execute(const char *path)
{
    if (!path || *path == '\0')
    {
        throw std::invalid_argument("Program path cannot be empty");
    }

    std::system(path);
}

inline const std::string avErrorText(int code)
{
    char buf[AV_ERROR_MAX_STRING_SIZE] = {0};
    if (av_strerror(code, buf, sizeof(buf)) == 0)
        return buf;
    return "Unknown error";
}

#include <sstream>
#include <iomanip>

inline std::string bytes(uint8_t *data, uint32_t length, uint16_t max)
{
    std::ostringstream oss;

    if (data && length > 0)
    {
        for (uint32_t i = 0; (i < length) && (i < max); ++i)
        {
            oss << std::setw(4) << static_cast<uint32_t>(data[i]);
        }
    }

    return oss.str();
}

#endif /* SRC_COMMON_FUNCTIONS */
