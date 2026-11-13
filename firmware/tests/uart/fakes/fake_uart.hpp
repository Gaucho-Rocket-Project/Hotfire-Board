#pragma once
#include <algorithm>
#include <cassert>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <vector>

struct device { bool ready = true; };
inline bool device_is_ready(const device* dev) { return dev->ready; }
using atomic_t = int;
inline int atomic_get(const atomic_t* p) { return *p; }
inline int atomic_set(atomic_t* p, int value) { int old = *p; *p = value; return old; }
inline int atomic_clear(atomic_t* p) { return atomic_set(p, 0); }
inline bool atomic_cas(atomic_t* p, int old, int value) {
    if (*p != old) { return false; } *p = value; return true;
}
struct ring_buf { std::deque<uint8_t> bytes; size_t capacity = 0; };
inline void ring_buf_init(ring_buf* r, size_t size, uint8_t*) { r->bytes.clear(); r->capacity = size; }
inline uint32_t ring_buf_put(ring_buf* r, const uint8_t* data, uint32_t size) {
    size = std::min(size, static_cast<uint32_t>(r->capacity - r->bytes.size()));
    r->bytes.insert(r->bytes.end(), data, data + size); return size;
}
inline uint32_t ring_buf_get(ring_buf* r, uint8_t* data, uint32_t size) {
    size = std::min(size, static_cast<uint32_t>(r->bytes.size()));
    for (uint32_t i = 0; i < size; ++i) { data[i] = r->bytes.front(); r->bytes.pop_front(); }
    return size;
}
struct k_msgq { size_t size, capacity; std::deque<std::vector<uint8_t>> messages; };
inline void k_msgq_init(k_msgq* q, char*, size_t size, size_t capacity) {
    q->size = size; q->capacity = capacity; q->messages.clear();
}
inline int k_msgq_put(k_msgq* q, const void* data, int) {
    if (q->messages.size() == q->capacity) { return -ENOMSG; }
    auto* p = static_cast<const uint8_t*>(data);
    q->messages.emplace_back(p, p + q->size); return 0;
}
inline int k_msgq_get(k_msgq* q, void* data, int) {
    if (q->messages.empty()) { return -ENOMSG; }
    memcpy(data, q->messages.front().data(), q->size); q->messages.pop_front(); return 0;
}
struct k_work { void (*handler)(k_work*) = nullptr; };
struct k_work_delayable { k_work work; };
inline std::deque<k_work*> pending_work;
inline void k_work_init_delayable(k_work_delayable* w, void (*fn)(k_work*)) { w->work.handler = fn; }
inline k_work_delayable* k_work_delayable_from_work(k_work* w) { return reinterpret_cast<k_work_delayable*>(w); }
inline int k_work_reschedule(k_work_delayable* w, int) {
    if (std::find(pending_work.begin(), pending_work.end(), &w->work) == pending_work.end()) {
        pending_work.push_back(&w->work);
    }
    return 1;
}
inline void run_work() {
    assert(!pending_work.empty());
    auto* w = pending_work.front(); pending_work.pop_front(); w->handler(w);
}
inline void drain_work() {
    unsigned n = 0;
    while (!pending_work.empty()) { assert(++n < 100); run_work(); }
}
#define K_NO_WAIT 0
#define K_MSEC(ms) (ms)
#define CONTAINER_OF(ptr, type, member) reinterpret_cast<type*>(reinterpret_cast<char*>(ptr) - offsetof(type, member))
#define LOG_LEVEL_INF 3
#define LOG_MODULE_REGISTER(...)
template<class... Args> void fake_log(const char*, Args...) {}
#define LOG_WRN(...) fake_log(__VA_ARGS__)

enum uart_event_type { UART_TX_DONE, UART_TX_ABORTED, UART_RX_RDY, UART_RX_BUF_REQUEST,
                       UART_RX_BUF_RELEASED, UART_RX_DISABLED, UART_RX_STOPPED };
struct uart_event {
    uart_event_type type;
    struct {
        struct { const uint8_t* buf; size_t len; } tx{};
        struct { uint8_t* buf; size_t offset, len; } rx{};
        struct { uint8_t* buf; } rx_buf{};
    } data{};
};
using uart_callback_t = void (*)(const device*, uart_event*, void*);
struct FakeUart {
    const device* dev{};
    uart_callback_t callback{};
    void* context{};
    const uint8_t* tx{};
    size_t tx_length{};
    uint8_t *rx{}, *next{};
    int callback_error{}, tx_error{}, rx_error{}, rsp_error{};
    unsigned tx_calls{}, rx_calls{}, rsp_calls{};
    bool synchronous_tx = false;
} inline fake;
inline void emit(uart_event event) { fake.callback(fake.dev, &event, fake.context); }
inline int uart_callback_set(const device* dev, uart_callback_t cb, void* context) {
    if (fake.callback_error) { return fake.callback_error; }
    fake.dev = dev; fake.callback = cb; fake.context = context; return 0;
}
inline int uart_rx_enable(const device*, uint8_t* buf, size_t size, int32_t timeout) {
    ++fake.rx_calls; assert(size == 128 && timeout == 1000);
    if (fake.rx_error) { return fake.rx_error; }
    assert(fake.rx == nullptr); fake.rx = buf;
    emit({UART_RX_BUF_REQUEST}); return 0;
}
inline int uart_rx_buf_rsp(const device*, uint8_t* buf, size_t size) {
    ++fake.rsp_calls; assert(size == 128 && buf != fake.rx && !fake.next);
    if (fake.rsp_error) { return fake.rsp_error; }
    fake.next = buf; return 0;
}
inline void release(uint8_t* buf) {
    uart_event e{UART_RX_BUF_RELEASED}; e.data.rx_buf.buf = buf; emit(e);
}
inline int uart_rx_disable(const device*) {
    if (fake.rx) { release(fake.rx); }
    if (fake.next) { release(fake.next); }
    fake.rx = fake.next = nullptr; emit({UART_RX_DISABLED}); return 0;
}
inline void complete_tx(bool aborted = false) {
    assert(fake.tx);
    uart_event e{aborted ? UART_TX_ABORTED : UART_TX_DONE};
    e.data.tx.buf = fake.tx; e.data.tx.len = aborted ? 1 : fake.tx_length;
    fake.tx = nullptr; emit(e);
}
inline int uart_tx(const device*, const uint8_t* buf, size_t size, int32_t timeout) {
    ++fake.tx_calls; assert(timeout == 100000 && !fake.tx);
    if (fake.tx_error) { return fake.tx_error; }
    fake.tx = buf; fake.tx_length = size;
    if (fake.synchronous_tx) { complete_tx(); }
    return 0;
}
inline void receive(size_t offset, const char* data, size_t size) {
    assert(fake.rx && offset + size <= 128);
    memcpy(fake.rx + offset, data, size);
    uart_event e{UART_RX_RDY}; e.data.rx = {fake.rx, offset, size}; emit(e);
}
inline void rollover() {
    assert(fake.rx && fake.next);
    release(fake.rx); fake.rx = fake.next; fake.next = nullptr;
    emit({UART_RX_BUF_REQUEST});
}
