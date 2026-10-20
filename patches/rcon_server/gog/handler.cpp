#include "handler.hpp"

#include "layout.hpp"

namespace fc::core_plugin::rcon_server::gog {

Handler::Handler(const fc::CreateContext &context) noexcept : service_(game_, context.log()) {}

void Handler::plan(fc::Plan &plan) {
    game_.plan(plan, make_layout());
}

fc::Result<> Handler::prepare(fc::PrepareContext &) {
    // Resolve game data before listener setup reads the port and client login reads the password.
    if (auto ready = game_.prepare(); !ready) {
        return ready;
    }
    return service_.prepare();
}

void Handler::activate(fc::ActivateContext &) noexcept {
    service_.activate();
}

void Handler::write_status(fc::StatusContext &status) const noexcept {
    service_.write_status(status);
}

} // namespace fc::core_plugin::rcon_server::gog
