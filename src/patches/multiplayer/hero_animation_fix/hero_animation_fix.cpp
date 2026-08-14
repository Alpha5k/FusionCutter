#include "hero_animation_fix.hpp"

#include <cstddef>

namespace fusioncutter::patches::hero_animation_fix {
namespace {

constexpr std::size_t kOwnerOffset = 0x6C;
constexpr std::size_t kComboOffset = 0x114;
constexpr std::size_t kInputQueueOffset = 0x134;
constexpr std::size_t kInputDownOffset = 0x42;
constexpr std::size_t kStateOffset = 0x180;
constexpr std::size_t kControllableOffset = 0x240;
constexpr std::size_t kPlayerHandleOffset = 0xD4;
constexpr std::size_t kAliveFlagsOffset = 0x1FC;
constexpr std::uint8_t kAliveFlag = 1U << 3;
constexpr std::size_t kWeaponArrayOffset = 0x720;
constexpr std::size_t kFallbackWeaponIndexOffset = 0x740;
constexpr std::size_t kPackedSelectionOffset = 0x742;
constexpr std::size_t kAnimatorOffset = 0x750;
constexpr int kWeaponSlots = 8;

struct PredictionWindow {
    HeroAnimationFix* patch{};
    void* weapon{};
    HeroIdentity identity{};
    bool recording{};
};

struct AuthorityWindow {
    HeroAnimationFix* patch{};
    void* weapon{};
    int previous_state{};
    bool recording{};
};

template <typename Window, std::size_t Capacity> struct WindowStack {
    [[nodiscard]] bool push(Window window) noexcept {
        if (size == values.size()) {
            return false;
        }
        values[size++] = window;
        return true;
    }

    [[nodiscard]] Window* current() noexcept {
        return size == 0 ? nullptr : &values[size - 1];
    }

    void pop() noexcept {
        if (size != 0) {
            --size;
        }
    }

    std::array<Window, Capacity> values{};
    std::size_t size{};
};

thread_local WindowStack<PredictionWindow, 4> gPredictionWindows;
thread_local WindowStack<AuthorityWindow, 4> gAuthorityWindows;
thread_local std::size_t gPredictionOverflow{};
thread_local std::size_t gAuthorityOverflow{};

[[nodiscard]] constexpr bool valid_state(int state) noexcept {
    return state >= 0 && state < 32;
}

} // namespace

HeroAnimationFix::HeroAnimationFix(const TargetContext& target) noexcept
    : layout_(layout_for(target.layout)), image_(target.image),
      get_joystick_index_(image_.function_at_rva<GetJoystickIndex>(layout_.joystick_lookup.rva)),
      prediction_turn_(image_.read_at_rva<volatile std::int32_t>(layout_.state.prediction_turn_rva)),
      acknowledged_turn_(image_.read_at_rva<volatile std::int32_t>(layout_.state.acknowledged_turn_rva)),
      is_local_turn_(image_.read_at_rva<volatile std::uint8_t>(layout_.state.is_local_turn_rva)),
      is_update_turn_(image_.read_at_rva<volatile std::uint8_t>(layout_.state.is_update_turn_rva)),
      network_enabled_(image_.read_at_rva<volatile std::uint8_t>(layout_.state.network_enabled_rva)),
      network_client_active_(image_.read_at_rva<volatile std::uint8_t>(layout_.state.network_client_active_rva)),
      prediction_resume_(image_.address_at_rva(layout_.native.prediction_resume_rva)) {
    pipeline_callbacks_ = {
        .context = this,
        .before_update =
            [](void* context, void* weapon, float delta) noexcept {
                return static_cast<HeroAnimationFix*>(context)->begin_update(weapon, delta);
            },
        .after_update =
            [](void* context, void* weapon, float delta, bool native_called, bool result) noexcept {
                static_cast<HeroAnimationFix*>(context)->finish_update(weapon, delta, native_called, result);
            },
        .before_network_state =
            [](void* context, void* weapon, int state, bool flag) noexcept {
                return static_cast<HeroAnimationFix*>(context)->begin_network_state(weapon, state, flag);
            },
        .after_network_state =
            [](void* context, void* weapon, int state, bool flag, bool native_called) noexcept {
                static_cast<HeroAnimationFix*>(context)->finish_network_state(weapon, state, flag, native_called);
            },
        .enter_state =
            [](void* context, void* weapon, int state) noexcept {
                static_cast<HeroAnimationFix*>(context)->observe_enter_state(weapon, state);
            },
        .suppress_animator_state =
            [](void* context, void* animator, std::uint32_t weapon_state, std::uint32_t active,
               std::uint32_t primary_animation, std::uint32_t secondary_animation) noexcept {
                return static_cast<HeroAnimationFix*>(context)->suppress_animator_state(
                    animator, weapon_state, active, primary_animation, secondary_animation);
            },
        .input_queue =
            [](void* context, MidHookContext& hook) noexcept {
                static_cast<HeroAnimationFix*>(context)->reconcile_input_queue(hook);
            },
        .prediction_transition =
            [](void* context, MidHookContext& hook) noexcept {
                static_cast<HeroAnimationFix*>(context)->suppress_authority_replay(hook);
            },
    };
}

void HeroAnimationFix::build_plan(PatchPlan& plan) {
    add_layout_requirements(plan, image_, layout_);
}

void HeroAnimationFix::enable_runtime() noexcept {
    clear_tracking();
    hero_melee_pipeline::publish_policy(pipeline_callbacks_);
}

void HeroAnimationFix::disable_runtime() noexcept {
    hero_melee_pipeline::clear_policy(pipeline_callbacks_);
    clear_tracking();
}

// Leaves one native prediction owner by omitting the duplicate authority/history update pass.
hero_melee_pipeline::UpdateDecision HeroAnimationFix::begin_update(void* weapon, float) noexcept {
    if (!gPredictionWindows.push({.patch = this, .weapon = weapon})) {
        ++gPredictionOverflow;
    }

    const auto active_prediction = network_prediction_active();
    bool ready{};
    const auto identity = capture_identity(weapon, ready);
    if (!identity.valid()) {
        clear_weapon(reinterpret_cast<std::uintptr_t>(weapon));
        return {};
    }
    if (!ready) {
        clear_identity(identity);
        return {};
    }

    if (active_prediction && *is_update_turn_ != 0 && *is_local_turn_ == 0) {
        return {.call_native = false, .result = true};
    }
    if (!active_prediction || *is_local_turn_ == 0) {
        return {};
    }

    if (gPredictionOverflow == 0) {
        auto* window = gPredictionWindows.current();
        window->identity = identity;
        window->recording = true;
        history_.begin_prediction(identity);
    }
    return {};
}

void HeroAnimationFix::finish_update(void* weapon, float, bool, bool) noexcept {
    if (gPredictionOverflow != 0) {
        --gPredictionOverflow;
        return;
    }

    const auto* window = gPredictionWindows.current();
    if (window == nullptr || window->patch != this || window->weapon != weapon) {
        return;
    }
    if (window->recording) {
        history_.finish_prediction(window->identity);
    }
    gPredictionWindows.pop();
}

// Applies unknown or corrective authority natively while rejecting only proven historical states.
hero_melee_pipeline::NetworkStateDecision HeroAnimationFix::begin_network_state(void* weapon, int state,
                                                                                bool) noexcept {
    if (!gAuthorityWindows.push({.patch = this, .weapon = weapon})) {
        ++gAuthorityOverflow;
    }

    if (!network_prediction_active()) {
        return {state};
    }

    bool ready{};
    const auto identity = capture_identity(weapon, ready);
    if (!identity.valid()) {
        clear_weapon(reinterpret_cast<std::uintptr_t>(weapon));
        return {state};
    }
    if (!ready) {
        clear_identity(identity);
        return {state};
    }

    const auto current_state = read_native_field<int>(weapon, kStateOffset);
    const auto action = history_.classify_authority(identity, current_state, state, *acknowledged_turn_);
    if (identity.is_local() && !history_.local_action_active(identity)) {
        presentations_[identity.local_player] = {};
    }
    if (action == AuthorityAction::SuppressHistorical) {
        return {.state = state, .call_native = false};
    }

    if (gAuthorityOverflow == 0) {
        auto* window = gAuthorityWindows.current();
        window->previous_state = current_state;
        window->recording = true;
    }
    return {state};
}

void HeroAnimationFix::finish_network_state(void* weapon, int state, bool, bool native_called) noexcept {
    if (gAuthorityOverflow != 0) {
        --gAuthorityOverflow;
        return;
    }

    const auto* window = gAuthorityWindows.current();
    if (window == nullptr || window->patch != this || window->weapon != weapon) {
        return;
    }
    if (native_called && window->recording && window->previous_state != state && valid_state(state) &&
        read_native_field<int>(weapon, kStateOffset) == state) {
        bool ready{};
        const auto identity = capture_identity(weapon, ready);
        history_.record_authority_transition(identity, state);
    }
    gAuthorityWindows.pop();
}

// Records native prediction transitions without replacing the game's state machine.
void HeroAnimationFix::observe_enter_state(void* weapon, int state) noexcept {
    const auto* authority = gAuthorityWindows.current();
    const auto* prediction = gPredictionWindows.current();
    if ((authority == nullptr || authority->weapon != weapon) && prediction != nullptr && prediction->patch == this &&
        prediction->recording && prediction->weapon == weapon) {
        observe_prediction_transition(weapon, state);
    }
}

bool HeroAnimationFix::suppress_animator_state(void* animator, std::uint32_t weapon_state, std::uint32_t active,
                                               std::uint32_t primary_animation,
                                               std::uint32_t secondary_animation) noexcept {
    if (animator == nullptr || weapon_state != 0 || active != 0 || primary_animation != 0xA4 ||
        secondary_animation != 0xA4 || !network_prediction_active()) {
        return false;
    }

    for (std::size_t local_player{}; local_player < presentations_.size(); ++local_player) {
        auto& presentation = presentations_[local_player];
        if (presentation.animator != animator) {
            continue;
        }

        auto* weapon = reinterpret_cast<void*>(presentation.identity.weapon);
        bool ready{};
        const auto identity = capture_identity(weapon, ready);
        if (!ready || identity != presentation.identity) {
            clear_identity(presentation.identity);
            return false;
        }

        const auto current_state = read_native_field<int>(weapon, kStateOffset);
        return history_.should_suppress_local_presentation(identity, current_state, *acknowledged_turn_);
    }
    return false;
}

HeroIdentity HeroAnimationFix::capture_identity(void* weapon, bool& ready) const noexcept {
    ready = false;
    if (weapon == nullptr || get_joystick_index_ == nullptr) {
        return {};
    }

    auto* owner = read_native_field<void*>(weapon, kOwnerOffset);
    auto* combo = read_native_field<void*>(weapon, kComboOffset);
    if (owner == nullptr || combo == nullptr) {
        return {};
    }

    const auto player_handle = read_native_field<int>(owner, kPlayerHandleOffset);
    if (player_handle < 0 || player_handle >= static_cast<int>(kNetworkPlayers)) {
        return {};
    }
    const auto local_player = get_joystick_index_(player_handle);
    const HeroIdentity identity{
        .weapon = reinterpret_cast<std::uintptr_t>(weapon),
        .owner = reinterpret_cast<std::uintptr_t>(owner),
        .combo = reinterpret_cast<std::uintptr_t>(combo),
        .player_handle = static_cast<std::int16_t>(player_handle),
        .local_player = local_player >= 0 && local_player < static_cast<int>(kLocalPlayers)
                            ? static_cast<std::uint8_t>(local_player)
                            : static_cast<std::uint8_t>(0xFF),
    };

    auto* soldier = static_cast<std::byte*>(owner) - kControllableOffset;
    const auto alive = (read_native_field<std::uint8_t>(soldier, kAliveFlagsOffset) & kAliveFlag) != 0;
    ready = identity.valid() && alive && selected_weapon(identity) == weapon;
    return identity;
}

void* HeroAnimationFix::selected_weapon(const HeroIdentity& identity) noexcept {
    if (!identity.valid()) {
        return nullptr;
    }
    auto* soldier = reinterpret_cast<std::byte*>(identity.owner) - kControllableOffset;
    const auto packed = read_native_field<std::uint8_t>(soldier, kPackedSelectionOffset);
    auto index = static_cast<int>(packed & 0x0F);
    if ((index & 0x08) != 0) {
        index -= 16;
    }
    if (index < 0 || index >= kWeaponSlots) {
        index = read_native_field<std::int8_t>(soldier, kFallbackWeaponIndexOffset);
    }
    if (index < 0 || index >= kWeaponSlots) {
        return nullptr;
    }
    return read_native_field<void*>(soldier, kWeaponArrayOffset + static_cast<std::size_t>(index) * sizeof(void*));
}

bool HeroAnimationFix::network_prediction_active() noexcept {
    const auto active = network_enabled_ != nullptr && network_client_active_ != nullptr &&
                        prediction_turn_ != nullptr && acknowledged_turn_ != nullptr && is_local_turn_ != nullptr &&
                        is_update_turn_ != nullptr && *network_enabled_ != 0 && *network_client_active_ != 0 &&
                        *prediction_turn_ > 0 && *acknowledged_turn_ >= 0;
    if (active) {
        prediction_was_active_ = true;
        return true;
    }
    if (prediction_was_active_) {
        clear_tracking();
    }
    prediction_was_active_ = false;
    return false;
}

void HeroAnimationFix::observe_prediction_transition(void* weapon, int state) noexcept {
    if (!valid_state(state)) {
        return;
    }
    const auto* window = gPredictionWindows.current();
    if (window == nullptr || window->patch != this ||
        window->identity.weapon != reinterpret_cast<std::uintptr_t>(weapon)) {
        return;
    }
    const auto& identity = window->identity;
    const auto base_state = read_native_field<int>(weapon, kStateOffset);
    if (!history_.observe_prediction(identity, base_state, state, *prediction_turn_)) {
        return;
    }
    if (identity.is_local()) {
        auto* soldier = reinterpret_cast<std::byte*>(identity.owner) - kControllableOffset;
        presentations_[identity.local_player] = {identity, read_native_field<void*>(soldier, kAnimatorOffset)};
    }
}

void HeroAnimationFix::clear_identity(const HeroIdentity& identity) noexcept {
    history_.clear(identity);
    if (identity.is_local()) {
        presentations_[identity.local_player] = {};
    }
}

void HeroAnimationFix::clear_weapon(std::uintptr_t weapon) noexcept {
    history_.clear_weapon(weapon);
    for (auto& presentation : presentations_) {
        if (presentation.identity.weapon == weapon) {
            presentation = {};
        }
    }
}

void HeroAnimationFix::clear_tracking() noexcept {
    history_.clear_all();
    presentations_ = {};
    prediction_was_active_ = false;
}

void HeroAnimationFix::reconcile_input_queue(MidHookContext& context) noexcept {
    const auto* window = gPredictionWindows.current();
    if (window == nullptr || window->patch != this || context.ecx < kInputQueueOffset) {
        return;
    }

    const auto weapon = context.ecx - kInputQueueOffset;
    if (window->identity.weapon != weapon || !window->identity.is_remote()) {
        return;
    }
    const auto buttons = read_native_field<std::uint8_t>(reinterpret_cast<const void*>(context.esp + sizeof(void*)));
    auto* queue = reinterpret_cast<void*>(context.ecx);
    auto down = read_native_field<std::uint8_t>(queue, kInputDownOffset);
    if (history_.reconcile_input(window->identity, buttons, down)) {
        write_native_field(queue, kInputDownOffset, down);
    }
}

void HeroAnimationFix::suppress_authority_replay(MidHookContext& context) noexcept {
    const auto* window = gPredictionWindows.current();
    if (window == nullptr || window->patch != this || window->identity.weapon != context.ebx) {
        return;
    }
    if (history_.resolve_replay(window->identity, static_cast<int>(context.esi))) {
        context.eip = prediction_resume_;
    }
}

} // namespace fusioncutter::patches::hero_animation_fix
