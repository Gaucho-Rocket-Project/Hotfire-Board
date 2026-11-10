#ifndef APP_TELEMETRY_H
#define APP_TELEMETRY_H

#include <zephyr/kernel.h>

class UART;

class TelemetryTask {
public:
    TelemetryTask(UART& uart, k_mutex& tx_mutex);

    int start();

private:
    struct WorkContext {
        k_work_delayable delayed{};
        TelemetryTask* owner = nullptr;
    };

    WorkContext work_{};
    UART& uart_;
    k_mutex& tx_mutex_;

    static void work_handler(struct k_work* work);
};

#endif