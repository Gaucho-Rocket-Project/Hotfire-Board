
#include "app/telemetry.hpp"
#include "drivers/uart.hpp"

#include <zephyr/sys/util.h>
#include <stdint.h>

TelemetryTask::TelemetryTask(
    UART& uart,
    k_mutex& tx_mutex)
    : uart_(uart),
      tx_mutex_(tx_mutex)
{
    work_.owner = this;

    k_work_init_delayable(
        &work_.delayed,
        work_handler
    );
}

int TelemetryTask::start()
{
    return k_work_schedule(
        &work_.delayed,
        K_SECONDS(1)
    );
}

void TelemetryTask::work_handler(
    struct k_work* work)
{
    auto* delayed =
        k_work_delayable_from_work(work);

    auto* context =
        CONTAINER_OF(delayed, WorkContext, delayed);

    auto* self = context->owner;

    static constexpr char message[] =
        "heartbeat\r\n";

    // Synchronize with any other UART transmitters.
    k_mutex_lock(&self->tx_mutex_, K_FOREVER);

    self->uart_.write(
        reinterpret_cast<const uint8_t*>(message),
        sizeof(message) - 1
    );

    k_mutex_unlock(&self->tx_mutex_);

    // Schedule the next transmission.
    (void)k_work_reschedule(
        &context->delayed,
        K_SECONDS(1)
    );
}
