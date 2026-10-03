#ifndef SRC_STRUCT_ATOMIC_QUEUE
#define SRC_STRUCT_ATOMIC_QUEUE

#include <cstddef>
#include <cstdint>
#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>

using namespace std;

// A small bounded FIFO of unique_ptr slots. Pushes and pops take the mutex --
// at the queue's rates (frames/segments, not samples) that costs nothing, and
// it makes the queue safe for several producers (two CarPlay audio streams
// feed one aux queue) while closing the classic lost-wakeup window: the old
// lock-free push published its state and notified OUTSIDE the mutex the
// waiter's predicate ran under, so a consumer could re-check, miss the new
// item, and sleep until its timeout. `_count` stays atomic so has()/count()
// remain cheap lock-free probes from other threads.
template <typename T>
class AtomicQueue
{
public:
    AtomicQueue(uint16_t size)
        : _size(size), _data(new unique_ptr<T>[size]), _first(0), _last(0), _count(0)
    {
    }

    AtomicQueue(const AtomicQueue &) = delete;
    AtomicQueue &operator=(const AtomicQueue &) = delete;

    ~AtomicQueue() = default;

    // Push; when full the NEW item is dropped (returns false). Right for
    // input/command queues where earlier events must not be reordered away.
    bool pushDiscard(unique_ptr<T> obj)
    {
        {
            lock_guard<std::mutex> lock(_mtx);
            if (_count.load(std::memory_order_relaxed) == _size)
                return false;
            _first = (_first + 1) % _size;
            _data[_first] = std::move(obj);
            _count.fetch_add(1, std::memory_order_release);
        }
        _lock.notify_one();
        return true;
    }

    // Push; when full the OLDEST queued item is dropped to make room. Right
    // for live AV streams: the newest data is the only data worth showing,
    // and a stalled consumer must cost latency, not block fresh frames.
    // Returns false when something was dropped (so a video producer can ask
    // the source for a keyframe).
    bool pushDropOldest(unique_ptr<T> obj)
    {
        bool dropped = false;
        {
            lock_guard<std::mutex> lock(_mtx);
            if (_count.load(std::memory_order_relaxed) == _size)
            {
                _last = (_last + 1) % _size;
                _data[_last].reset();
                _count.fetch_sub(1, std::memory_order_release);
                dropped = true;
            }
            _first = (_first + 1) % _size;
            _data[_first] = std::move(obj);
            _count.fetch_add(1, std::memory_order_release);
        }
        _lock.notify_one();
        return !dropped;
    }

    // Push; when full the NEWEST queued item is replaced in place.
    bool pushReplace(unique_ptr<T> obj)
    {
        bool replaced = false;
        {
            lock_guard<std::mutex> lock(_mtx);
            if (_count.load(std::memory_order_relaxed) == _size)
            {
                _data[_first] = std::move(obj);
                replaced = true;
            }
            else
            {
                _first = (_first + 1) % _size;
                _data[_first] = std::move(obj);
                _count.fetch_add(1, std::memory_order_release);
            }
        }
        if (!replaced)
            _lock.notify_one();
        return !replaced;
    }

    const T *peek()
    {
        lock_guard<std::mutex> lock(_mtx);
        if (_count.load(std::memory_order_relaxed) == 0)
            return nullptr;
        return _data[(_last + 1) % _size].get();
    }

    unique_ptr<T> pop()
    {
        lock_guard<std::mutex> lock(_mtx);
        if (_count.load(std::memory_order_relaxed) == 0)
            return nullptr;

        _last = (_last + 1) % _size;
        auto item = std::move(_data[_last]);
        _count.fetch_sub(1, std::memory_order_release);
        return item;
    }

    bool has(uint16_t count)
    {
        return _count.load(std::memory_order_acquire) >= count;
    }

    bool wait(atomic<bool> &waitFlag, uint16_t count = 0)
    {
        unique_lock<std::mutex> lock(_mtx);

        _lock.wait(lock, [&]
                   { return _count.load(std::memory_order_acquire) > count || !waitFlag.load(std::memory_order_acquire); });
        return waitFlag.load(std::memory_order_acquire);
    }

    bool waitFor(atomic<bool> &waitFlag, uint32_t timeoutMs, uint16_t count = 0)
    {
        unique_lock<std::mutex> lock(_mtx);
        _lock.wait_for(lock, std::chrono::milliseconds(timeoutMs), [&]
                       { return _count.load(std::memory_order_acquire) > count || !waitFlag.load(std::memory_order_acquire); });
        return waitFlag.load(std::memory_order_acquire);
    }

    void clear()
    {
        lock_guard<std::mutex> lock(_mtx);
        // Reset the slots in place: replacing the whole array (the previous
        // behaviour) re-allocated _size pointers on an audio-thread path.
        for (uint16_t i = 0; i < _size; i++)
            _data[i].reset();
        _first = 0;
        _last = 0;
        _count.store(0, std::memory_order_release);
    }

    void notify()
    {
        _lock.notify_all();
    }

    uint16_t count() const { return _count.load(std::memory_order_acquire); }

private:
    uint16_t _size;
    unique_ptr<unique_ptr<T>[]> _data;
    uint16_t _first;
    uint16_t _last;
    atomic<uint16_t> _count;
    mutex _mtx;
    condition_variable _lock;
};

#endif /* SRC_STRUCT_ATOMIC_QUEUE */
