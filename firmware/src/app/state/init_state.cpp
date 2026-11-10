#include "app/fsm.hpp"

smf_state_result FSM::init_run(void* obj)
{
    auto& ctx = context(obj);

    ctx.last_error = ctx.app->led_.init();

    if (ctx.last_error < 0) {
        smf_set_initial(
            &ctx.smf,
            &states_[static_cast<int>(State::ERROR)]
        );
    } else {
        smf_set_initial(
            &ctx.smf,
            &states_[static_cast<int>(State::LED_ON)]
        );
    }

    return SMF_EVENT_HANDLED;
}