#include "app/fsm.hpp"

void FSM::led_off_entry(void* obj)
{
    auto& ctx = context(obj);

    ctx.last_error = ctx.app->led_.off();
    ctx.entered_at_ms = k_uptime_get();
}

smf_state_result FSM::led_off_run(void* obj)
{
    auto& ctx = context(obj);

    if (ctx.last_error < 0) {
        smf_set_state(&ctx.smf, &states_[static_cast<int>(State::ERROR)]);
    } else if (k_uptime_get() - ctx.entered_at_ms
               >= BLINK_INTERVAL_MS) {
        smf_set_state(&ctx.smf, &states_[static_cast<int>(State::LED_ON)]);
    }

    return SMF_EVENT_HANDLED;
}