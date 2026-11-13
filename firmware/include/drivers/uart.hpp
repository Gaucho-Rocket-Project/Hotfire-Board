#ifndef DRIVERS_UART_H
#define DRIVERS_UART_H

#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/ring_buffer.h>

#include <stddef.h>
#include <stdint.h>

struct uart_event;

class UART {
public:
    explicit UART(const device* const uart) noexcept : uart_(uart) {}
    UART(const UART&) = delete;
    UART& operator=(const UART&) = delete;

    int init();

    // Copies a complete message. 0 means queued, not physically transmitted.
    // Thread context only; -ENOSPC if full, -EMSGSIZE above 128 bytes.
    int write(const uint8_t* data, size_t length);

    // Returns 0 if a byte was received.
    // Returns -EAGAIN if empty, -EOVERFLOW if data was lost (resync required).
    int read_byte(uint8_t& byte);

private:
    // This object must outlive all DMA transfers and scheduled work.
    struct TxMessage {
        uint8_t bytes[128];
        size_t length;
    };
    struct WorkContext {
        k_work_delayable work{};
        UART* owner = nullptr;
    };
    const struct device* const uart_;
    ring_buf rx_{};
    uint8_t rx_storage_[512]{};
    uint8_t rx_dma_[2][128]{};
    atomic_t rx_owned_[2]{};
    atomic_t rx_error_{};
    atomic_t rx_stop_{};
    atomic_t ready_{};
    atomic_t tx_busy_{};
    k_msgq tx_queue_{};
    alignas(TxMessage) char tx_storage_[8 * sizeof(TxMessage)]{};
    TxMessage tx_active_{};
    WorkContext tx_work_{};
    WorkContext rx_work_{};

    int start_rx();
    static void event_handler(const device* dev, uart_event* event, void* context);
    static void tx_handler(k_work* work);
    static void rx_handler(k_work* work);
};

#endif // DRIVERS_UART_H
