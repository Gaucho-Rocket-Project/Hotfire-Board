#include "drivers/uart.hpp"

#include <zephyr/drivers/uart.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include <errno.h>
#include <string.h>

LOG_MODULE_REGISTER(daq_uart, LOG_LEVEL_INF);

int UART::init()
{
    if (atomic_get(&ready_)) {
        return -EALREADY;
    }
    if (!device_is_ready(uart_)) {
        return -ENODEV;
    }
    ring_buf_init(&rx_, sizeof(rx_storage_), rx_storage_);
    k_msgq_init(&tx_queue_, tx_storage_, sizeof(TxMessage), 8);
    tx_work_.owner = this;
    rx_work_.owner = this;
    k_work_init_delayable(&tx_work_.work, tx_handler);
    k_work_init_delayable(&rx_work_.work, rx_handler);
    int ret = uart_callback_set(uart_, event_handler, this);
    if (ret < 0) {
        return ret;
    }
    ret = start_rx();
    if (ret == 0) {
        atomic_set(&ready_, 1);
    }
    return ret;
}

int UART::write(const uint8_t* data, size_t length)
{
    if (!atomic_get(&ready_)) {
        return -ENODEV;
    }
    if (length > sizeof(TxMessage::bytes)) {
        return -EMSGSIZE;
    }
    if (length == 0) {
        return 0;
    }
    if (data == nullptr) {
        return -EINVAL;
    }
    TxMessage message{};
    memcpy(message.bytes, data, length);
    message.length = length;
    if (k_msgq_put(&tx_queue_, &message, K_NO_WAIT) != 0) {
        return -ENOSPC;
    }
    (void)k_work_reschedule(&tx_work_.work, K_NO_WAIT);
    return 0;
}

void UART::tx_handler(k_work* work)
{
    auto* context = CONTAINER_OF(k_work_delayable_from_work(work), WorkContext, work);
    auto* self = context->owner;
    if (atomic_get(&self->tx_busy_)) {
        return;
    }
    if (k_msgq_get(&self->tx_queue_, &self->tx_active_, K_NO_WAIT) != 0) {
        return;
    }
    atomic_set(&self->tx_busy_, 1);
    // Persistent storage is released only by TX_DONE/TX_ABORTED. The 100 ms
    // timeout exceeds a 128-byte message's ~11 ms wire time at 115200 baud.
    int ret = uart_tx(self->uart_, self->tx_active_.bytes,
                      self->tx_active_.length, 100000);
    if (ret < 0) {
        atomic_clear(&self->tx_busy_);
        LOG_WRN("USART1 TX start failed; message dropped: %d", ret);
        (void)k_work_reschedule(&self->tx_work_.work, K_MSEC(1));
    }
}

int UART::start_rx()
{
    for (unsigned i = 0; i < 2; ++i) {
        if (!atomic_cas(&rx_owned_[i], 0, 1)) {
            continue;
        }
        // Receive short commands without waiting for the whole buffer to fill.
        int ret = uart_rx_enable(uart_, rx_dma_[i], sizeof(rx_dma_[i]), 1000);
        if (ret < 0) {
            atomic_clear(&rx_owned_[i]);
        }
        return ret;
    }
    return -EBUSY;
}

void UART::rx_handler(k_work* work)
{
    auto* context = CONTAINER_OF(k_work_delayable_from_work(work), WorkContext, work);
    auto* self = context->owner;
    if (atomic_set(&self->rx_stop_, 0)) {
        // Some STM32 DMA errors report STOPPED without releasing buffers.
        // Disable in thread context; DISABLED schedules recovery after release.
        (void)uart_rx_disable(self->uart_);
        return;
    }
    int ret = self->start_rx();
    if (ret < 0) {
        (void)k_work_reschedule(&self->rx_work_.work, K_MSEC(10));
    }
}

void UART::event_handler(const device* dev, uart_event* event, void* context)
{
    auto* self = static_cast<UART*>(context);
    switch (event->type) {
    case UART_TX_ABORTED:
        // Do not replay a partially transmitted reply: the peer must retry it.
        LOG_WRN("USART1 TX aborted after %u bytes", (unsigned)event->data.tx.len);
        [[fallthrough]];
    case UART_TX_DONE:
        atomic_clear(&self->tx_busy_);
        (void)k_work_reschedule(&self->tx_work_.work, K_NO_WAIT);
        break;
    case UART_RX_RDY:
        if (ring_buf_put(&self->rx_, event->data.rx.buf + event->data.rx.offset,
                         event->data.rx.len) != event->data.rx.len) {
            atomic_set(&self->rx_error_, 1);
        }
        break;
    case UART_RX_BUF_REQUEST:
        for (unsigned i = 0; i < 2; ++i) {
            if (!atomic_cas(&self->rx_owned_[i], 0, 1)) {
                continue;
            }
            int ret = uart_rx_buf_rsp(dev, self->rx_dma_[i], sizeof(self->rx_dma_[i]));
            if (ret < 0) {
                atomic_clear(&self->rx_owned_[i]);
                atomic_set(&self->rx_error_, 1);
            }
            break;
        }
        break;
    case UART_RX_BUF_RELEASED:
        for (unsigned i = 0; i < 2; ++i) {
            if (event->data.rx_buf.buf == self->rx_dma_[i]) {
                atomic_clear(&self->rx_owned_[i]);
                break;
            }
        }
        break;
    case UART_RX_STOPPED:
        atomic_set(&self->rx_error_, 1);
        atomic_set(&self->rx_stop_, 1);
        (void)k_work_reschedule(&self->rx_work_.work, K_MSEC(1));
        break;
    case UART_RX_DISABLED:
        atomic_set(&self->rx_error_, 1);
        atomic_clear(&self->rx_stop_);
        (void)k_work_reschedule(&self->rx_work_.work, K_MSEC(1));
        break;
    default:
        break;
    }
}

int UART::read_byte(uint8_t& byte)
{
    if (atomic_set(&rx_error_, 0)) {
        // Consumer-side drain preserves the single-producer/single-consumer
        // contract while DMA callbacks may still be producing bytes.
        uint8_t discarded[32];
        for (unsigned i = 0; i < sizeof(rx_storage_) / sizeof(discarded); ++i) {
            if (ring_buf_get(&rx_, discarded, sizeof(discarded)) == 0) {
                break;
            }
        }
        return -EOVERFLOW;
    }
    return ring_buf_get(&rx_, &byte, 1) == 1 ? 0 : -EAGAIN;
}
