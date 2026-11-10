#include "app/fsm.hpp"

void FSM::error_entry(void* obj)
{
    auto& ctx = context(obj);

    // Best effort: turn LED off.
    ctx.app->led_.off();

    printk("Application error: %d\n", ctx.last_error);
}

smf_state_result FSM::error_run(void* obj)
{
    return SMF_EVENT_HANDLED;
}