#include "app/fsm.hpp"

void FSM::led_on_entry(void* obj)
{
    auto& ctx = context(obj);

    ctx.last_error = ctx.app->led_.on();
    ctx.entered_at_ms = k_uptime_get();
}

smf_state_result FSM::led_on_run(void* obj)
{
    auto& ctx = context(obj);

    if (ctx.last_error < 0) {
        smf_set_initial(
            &ctx.smf,
            &states_[static_cast<int>(State::ERROR)]
        );
    } else if (k_uptime_get() - ctx.entered_at_ms
               >= BLINK_INTERVAL_MS) {
        smf_set_initial(
            &ctx.smf,
            &states_[static_cast<int>(State::LED_OFF)]
        );
    }

    return SMF_EVENT_HANDLED;
}