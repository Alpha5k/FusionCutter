#include "rcon.hpp"
#include "game.hpp"
#include "service.hpp"

#include <memory>

namespace fusioncutter::patches::rcon_server {

// Coordinates the game bridge and network service through the patch lifecycle.
class GogRcon::State {
  public:
    explicit State(ImageContext image) noexcept : game_(image), service_(game_) {}

    ~State() {
        disable();
    }

    void build_plan(PatchPlan& plan) {
        game_.build_plan(plan);
    }

    [[nodiscard]] std::expected<void, OutcomeReason> prepare() {
        return service_.prepare();
    }

    void enable() noexcept {
        // Publish the hook target before allowing the prepared worker to consume captured chat.
        game_.enable();
        service_.enable();
    }

    void disable() noexcept {
        // Close the game callback gate before joining and destroying service resources.
        game_.disable();
        service_.disable();
    }

  private:
    gog::Game game_;
    gog::Service service_;
};

GogRcon::GogRcon(const TargetContext& target) : state_(std::make_unique<State>(target.image)) {}
GogRcon::~GogRcon() = default;

void GogRcon::build_plan(PatchPlan& plan) {
    state_->build_plan(plan);
}

std::expected<void, OutcomeReason> GogRcon::prepare_runtime() {
    return state_->prepare();
}

void GogRcon::enable_runtime() noexcept {
    state_->enable();
}

void GogRcon::disable_runtime() noexcept {
    state_->disable();
}

} // namespace fusioncutter::patches::rcon_server
