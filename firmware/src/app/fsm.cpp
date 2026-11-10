#include "app/fsm.hpp"

FSM::FSM(LED& led)
    : led_(led)
{
    ctx_.app = this;
}

FSM::Context& FSM::context(void* obj)
{
    return *static_cast<Context*>(obj);
}

void FSM::start()
{
    smf_set_initial(
        &ctx_.smf,
        &states_[static_cast<int>(State::INIT)]
    );
}

int FSM::step()
{
    return smf_run_state(&ctx_.smf);
}

// ---------- STATE TABLE ----------

const struct smf_state FSM::states_[4] = {
    SMF_CREATE_STATE(nullptr, init_run, nullptr, nullptr, nullptr),
    SMF_CREATE_STATE(led_on_entry, led_on_run, nullptr, nullptr, nullptr),
    SMF_CREATE_STATE(led_off_entry, led_off_run, nullptr, nullptr, nullptr),
    SMF_CREATE_STATE(error_entry, error_run, nullptr, nullptr, nullptr),
};