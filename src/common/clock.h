#ifndef SRC_COMMON_CLOCK
#define SRC_COMMON_CLOCK

#include <cstdint>
#include <ctime>

// Monotonic wall-clock helpers (the former SDL_GetTicks/SDL_Delay users).
// CLOCK_MONOTONIC never steps with NTP/RTC changes, which matters for the
// timeouts and tick sources these feed.

inline uint32_t mono_ms()
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)((uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u);
}

inline void sleep_ms(unsigned ms)
{
    struct timespec ts{(time_t)(ms / 1000u), (long)(ms % 1000u) * 1000000L};
    while (nanosleep(&ts, &ts) != 0)
        ; // resume after a signal with the remaining time
}

#endif /* SRC_COMMON_CLOCK */
