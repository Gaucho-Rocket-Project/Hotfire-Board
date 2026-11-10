#ifndef APP_FSM_H
#define APP_FSM_H

#include <zephyr/smf.h>
#include <zephyr/kernel.h>

#include "drivers/led.hpp"

#define BLINK_INTERVAL_MS 2000

class FSM {
public:
    explicit FSM(LED& led);

    void start();
    int step();

private:
    enum class State {
        INIT,
        LED_ON,
        LED_OFF,
        ERROR,
    };

    static constexpr size_t to_index(State state)
    {
        return static_cast<size_t>(state);
    }

    struct Context {
        struct smf_ctx smf{};
        FSM* app = nullptr;
        int64_t entered_at_ms = 0;
        int last_error = 0;
    };

    Context ctx_{};
    LED& led_;

    static const struct smf_state states_[4];

    static Context& context(void* obj);

    // INIT
    static smf_state_result init_run(void* obj);

    // LED_ON
    static void led_on_entry(void* obj);
    static smf_state_result led_on_run(void* obj);

    // LED_OFF
    static void led_off_entry(void* obj);
    static smf_state_result led_off_run(void* obj);

    // ERROR
    static void error_entry(void* obj);
    static smf_state_result error_run(void* obj);
};

#endif // APP_FSM_H