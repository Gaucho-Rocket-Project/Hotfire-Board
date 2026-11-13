// c++ -std=c++17 -Wall -Wextra -Werror -Itests/uart/fakes -Iinclude \
//   tests/uart_dma_test.cpp src/drivers/uart.cpp -o /tmp/uart_dma_test && /tmp/uart_dma_test
#include <fake_uart.hpp>
#include "drivers/uart.hpp"
#include <iostream>
#include <string>

static void reset() { pending_work.clear(); fake = {}; }
static int send(UART& uart, const char* text) {
    return uart.write(reinterpret_cast<const uint8_t*>(text), strlen(text));
}
static std::string transmitted() {
    assert(fake.tx); return {reinterpret_cast<const char*>(fake.tx), fake.tx_length};
}
static std::string received(UART& uart) {
    std::string result; uint8_t byte;
    while (uart.read_byte(byte) == 0) { result += static_cast<char>(byte); }
    return result;
}
int main() {
    device dev;
    {
        UART uart(&dev);
        assert(send(uart, "early") == -ENODEV);
        dev.ready = false; assert(uart.init() == -ENODEV); dev.ready = true;
        fake.callback_error = -ENOTSUP; assert(uart.init() == -ENOTSUP);
        fake.callback_error = 0; fake.rx_error = -EIO; assert(uart.init() == -EIO);
        fake.rx_error = 0; assert(uart.init() == 0); assert(uart.init() == -EALREADY);
        assert(fake.rx && fake.next && fake.rx != fake.next);
        assert(uart.write(nullptr, 0) == 0);
        assert(uart.write(nullptr, 1) == -EINVAL);
        uint8_t oversized[129]{}; assert(uart.write(oversized, sizeof oversized) == -EMSGSIZE);

        // Caller storage can change immediately. Active DMA storage cannot change.
        char reply[] = "RTC TIME UNSET\r\n";
        assert(send(uart, reply) == 0); memset(reply, 'x', sizeof reply);
        run_work(); assert(transmitted() == "RTC TIME UNSET\r\n");
        for (int i = 0; i < 8; ++i) { assert(send(uart, "heartbeat\r\n") == 0); }
        assert(send(uart, "full") == -ENOSPC);
        drain_work(); assert(transmitted() == "RTC TIME UNSET\r\n");
        complete_tx(); run_work(); assert(transmitted() == "heartbeat\r\n");
        for (int i = 0; i < 8; ++i) { complete_tx(); drain_work(); }
        assert(!fake.tx);

        // Start failures and aborts release the active slot and advance the queue.
        assert(send(uart, "failed") == 0); assert(send(uart, "next") == 0);
        fake.tx_error = -EIO; run_work(); fake.tx_error = 0;
        run_work(); assert(transmitted() == "next");
        assert(send(uart, "after abort") == 0);
        complete_tx(true); drain_work(); assert(transmitted() == "after abort");
        complete_tx(); drain_work();
        fake.synchronous_tx = true;
        assert(send(uart, "immediate completion") == 0); drain_work(); assert(!fake.tx);
        fake.synchronous_tx = false;

        // Partial idle events must copy only the newly reported range.
        receive(0, "RTC ", 4); receive(4, "GET\n", 4);
        assert(received(uart) == "RTC GET\n");
        auto* first = fake.rx;
        rollover(); assert(fake.next == first && fake.rx != first);
        receive(0, "RTC SET ", 8); rollover();
        receive(0, "2026-11-12T00:00:00Z\n", 21);
        assert(received(uart) == "RTC SET 2026-11-12T00:00:00Z\n");

        // Exhausted consumer buffer invalidates the parser; subsequent input works.
        const std::string block(128, 'x');
        for (int i = 0; i < 5; ++i) { receive(0, block.data(), block.size()); rollover(); }
        uint8_t byte;
        assert(uart.read_byte(byte) == -EOVERFLOW);
        assert(uart.read_byte(byte) == -EAGAIN);
        receive(0, "\nRTC GET\n", 9); assert(received(uart) == "\nRTC GET\n");

        // STOPPED alone needs explicit disable before buffers can be reclaimed.
        emit({UART_RX_STOPPED}); run_work(); assert(!fake.rx && !fake.next);
        fake.rx_error = -EIO; run_work(); assert(!fake.rx);
        fake.rx_error = 0; run_work(); assert(fake.rx && fake.next);
        assert(uart.read_byte(byte) == -EOVERFLOW);
        receive(0, "RTC GET\n", 8); assert(received(uart) == "RTC GET\n");

        // Drivers that already emit DISABLED should not be disabled a second time.
        unsigned enables = fake.rx_calls;
        emit({UART_RX_STOPPED}); uart_rx_disable(&dev); drain_work();
        assert(fake.rx_calls == enables + 1 && fake.rx && fake.next);
        assert(uart.read_byte(byte) == -EOVERFLOW);

        // Failed next-buffer admission never marks an unowned buffer as in use.
        fake.rsp_error = -EIO; rollover(); assert(fake.next == nullptr);
        fake.rsp_error = 0; uart_rx_disable(&dev); drain_work();
        assert(fake.rx && fake.next);
        assert(uart.read_byte(byte) == -EOVERFLOW);
        receive(0, "ok", 2); assert(received(uart) == "ok");
    }
    reset();
    std::cout << "UART DMA queue, ownership, rollover, overflow, and recovery tests passed\n";
}
