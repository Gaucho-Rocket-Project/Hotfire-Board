// From firmware/: c++ -std=c++17 -Wall -Wextra -Werror -Iinclude
// tests/ring_buffer_test.cpp src/drivers/utils/buffer.cpp -o /tmp/ring_buffer_test
// Run: /tmp/ring_buffer_test
#include "drivers/utils/buffer.hpp"
#include "drivers/utils/buffer.hpp"

#include <cassert>
#include <deque>
#include <type_traits>

template<size_t N>
void check_fifo()
{
    RingBuffer<int, N> buffer;
    const auto& view = buffer;
    static_assert(std::is_same_v<decltype(view.front()), const int*>);
    std::deque<int> expected;
    uint32_t dropped = 0;

    assert(buffer.empty());
    assert(buffer.front() == nullptr);
    assert(!buffer.pop());
    assert(buffer.size() == 0);

    // Repeated fill, overwrite, partial drain, and refill across wrap boundaries.
    for (int round = 0; round < 100; ++round) {
        for (size_t i = 0; i < N + 2; ++i) {
            int value = round * 1000 + static_cast<int>(i);
            if (expected.size() == N) {
                expected.pop_front();
                ++dropped;
            }
            expected.push_back(value);
            buffer.push(value);
            assert(buffer.size() == expected.size());
            assert(*view.front() == expected.front());
            assert(buffer.dropped_count() == dropped);
        }

        size_t drain = round % 2 == 0 ? N / 2 : N;
        for (size_t i = 0; i < drain; ++i) {
            assert(*view.front() == expected.front());
            assert(buffer.pop());
            expected.pop_front();
        }
        assert(buffer.size() == expected.size());
        assert(buffer.empty() == expected.empty());
    }

    while (!expected.empty()) {
        assert(*view.front() == expected.front());
        assert(buffer.pop());
        expected.pop_front();
    }
    assert(view.front() == nullptr);
    assert(!buffer.pop());
    assert(buffer.size() == 0);
    assert(buffer.dropped_count() == dropped);

    buffer.push(42);
    assert(*buffer.front() == 42);
    assert(buffer.size() == 1);
}

int main()
{
    check_fifo<1>();
    check_fifo<3>();
    check_fifo<128>();

    struct Record { int value; char label[4]; };
    RingBuffer<Record, 2> records;
    records.push({7, "sd"});
    assert(records.front()->value == 7);
    assert(records.front()->label[0] == 's');
    assert(records.pop());
    assert(records.empty());
}
