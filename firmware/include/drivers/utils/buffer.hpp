#ifndef DRIVERS_UTILS_BUFFER_HPP
#define DRIVERS_UTILS_BUFFER_HPP

#include <stddef.h>
#include <stdint.h>

template<typename T, size_t N>
class RingBuffer {
    static_assert(N > 0, "RingBuffer capacity must be greater than zero");

public:
    void push(const T& item)
    {
        entries_[(head_ + count_) % N] = item;

        if (count_ == N) {
            head_ = (head_ + 1) % N;
            ++dropped_;
        } else {
            ++count_;
        }
    }

    T* front()
    {
        return empty() ? nullptr : &entries_[head_];
    }

    const T* front() const
    {
        return empty() ? nullptr : &entries_[head_];
    }

    bool pop()
    {
        if (empty()) {
            return false;
        }

        head_ = (head_ + 1) % N;
        --count_;
        return true;
    }

    bool empty() const { return count_ == 0; }
    size_t size() const { return count_; }
    uint32_t dropped_count() const { return dropped_; }

private:
    T entries_[N]{};
    size_t head_ = 0;
    size_t count_ = 0;
    uint32_t dropped_ = 0;
};

#endif // DRIVERS_UTILS_BUFFER_HPP
