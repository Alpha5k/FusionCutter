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
    HeroIdentity identity{};
};

thread_local PredictionWindow gPredictionWindow;
thread_local void* gAuthorityWeapon{};

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
      prediction_resume_(image_.address_at_rva(layout_.hooks.prediction_resume_rva)) {}

void HeroAnimationFix::build_plan(PatchPlan& plan) {
    add_layout_requirements(plan, image_, layout_);
    // The x86 detours use fastcall bridges while their original handles retain the native thiscall ABI.
    original_update_ = plan.inline_hook_with_original<UpdateFunction>(
        "Own hero melee prediction updates", layout_.hooks.update.rva, layout_.hooks.update.pattern(),
        reinterpret_cast<UpdateFunction>(&HeroAnimationFix::hook_update));
    original_set_network_state_ = plan.inline_hook_with_original<SetNetworkStateFunction>(
        "Reconcile authoritative hero melee states", layout_.hooks.set_network_state.rva,
        layout_.hooks.set_network_state.pattern(),
        reinterpret_cast<SetNetworkStateFunction>(&HeroAnimationFix::hook_set_network_state));
    original_enter_state_ = plan.inline_hook_with_original<EnterStateFunction>(
        "Record predicted hero melee transitions", layout_.hooks.enter_state.rva, layout_.hooks.enter_state.pattern(),
        reinterpret_cast<EnterStateFunction>(&HeroAnimationFix::hook_enter_state));
    original_animator_state_ = plan.inline_hook_with_original<AnimatorStateFunction>(
        "Preserve active local hero animations", layout_.hooks.animator_state.rva,
        layout_.hooks.animator_state.pattern(),
        reinterpret_cast<AnimatorStateFunction>(&HeroAnimationFix::hook_animator_state));

    const auto input_queue = input_queue_preimage(image_, layout_);
    plan.mid_hook("Reconcile remote hero input edges", layout_.hooks.input_queue_update.rva,
                  BytePattern::exact(input_queue), &HeroAnimationFix::reconcile_input_queue);
    plan.mid_hook("Suppress repeated authority-first hero transitions", layout_.hooks.prediction_transition.rva,
                  layout_.hooks.prediction_transition.pattern(), &HeroAnimationFix::suppress_authority_replay);
}

void HeroAnimationFix::enable_runtime() noexcept {
    clear_tracking();
    active_.publish(*this);
}

void HeroAnimationFix::disable_runtime() noexcept {
    active_.clear(*this);
    clear_tracking();
}

// Leaves one native prediction owner by omitting the duplicate authority/history update pass.
bool HeroAnimationFix::update_weapon(void* weapon, float delta) noexcept {
    const auto original = original_update_.get();
    if (original == nullptr) {
        return true;
    }

    const auto active_prediction = network_prediction_active();
    bool ready{};
    const auto identity = capture_identity(weapon, ready);
    if (!identity.valid()) {
        clear_weapon(reinterpret_cast<std::uintptr_t>(weapon));
        return original(weapon, delta);
    }
    if (!ready) {
        clear_identity(identity);
        return original(weapon, delta);
    }

    if (active_prediction && *is_update_turn_ != 0 && *is_local_turn_ == 0) {
        return true;
    }
    if (!active_prediction || *is_local_turn_ == 0) {
        return original(weapon, delta);
    }

    history_.begin_prediction(identity);
    const auto previous_window = gPredictionWindow;
    gPredictionWindow = {this, identity};
    const auto result = original(weapon, delta);
    history_.finish_prediction(identity);
    gPredictionWindow = previous_window;
    return result;
}

// Applies unknown or corrective authority natively while rejecting only proven historical states.
void HeroAnimationFix::apply_network_state(void* weapon, int state, bool flag) noexcept {
    const auto original = original_set_network_state_.get();
    if (original == nullptr) {
        return;
    }
    if (!network_prediction_active()) {
        original(weapon, state, flag);
        return;
    }

    bool ready{};
    const auto identity = capture_identity(weapon, ready);
    if (!identity.valid()) {
        clear_weapon(reinterpret_cast<std::uintptr_t>(weapon));
        original(weapon, state, flag);
        return;
    }
    if (!ready) {
        clear_identity(identity);
        original(weapon, state, flag);
        return;
    }

    const auto current_state = read_native_field<int>(weapon, kStateOffset);
    const auto action = history_.classify_authority(identity, current_state, state, *acknowledged_turn_);
    if (identity.is_local() && !history_.local_action_active(identity)) {
        presentations_[identity.local_player] = {};
    }
    if (action == AuthorityAction::SuppressHistorical) {
        return;
    }

    auto* previous_authority = gAuthorityWeapon;
    gAuthorityWeapon = weapon;
    original(weapon, state, flag);
    gAuthorityWeapon = previous_authority;

    if (current_state != state && valid_state(state) && read_native_field<int>(weapon, kStateOffset) == state) {
        history_.record_authority_transition(identity, state);
    }
}

// Records native prediction transitions without replacing the game's state machine.
void HeroAnimationFix::enter_state(void* weapon, int state) noexcept {
    if (gAuthorityWeapon != weapon && gPredictionWindow.patch == this &&
        gPredictionWindow.identity.weapon == reinterpret_cast<std::uintptr_t>(weapon)) {
        observe_prediction_transition(weapon, state);
    }
    if (const auto original = original_enter_state_.get(); original != nullptr) {
        original(weapon, state);
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
    const auto& identity = gPredictionWindow.identity;
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

bool __fastcall HeroAnimationFix::hook_update(void* weapon, void*, float delta) noexcept {
    if (auto* patch = active_.read(); patch != nullptr) {
        return patch->update_weapon(weapon, delta);
    }
    if (const auto original = original_update_.get(); original != nullptr) {
        return original(weapon, delta);
    }
    return true;
}

void __fastcall HeroAnimationFix::hook_set_network_state(void* weapon, void*, int state, bool flag) noexcept {
    if (auto* patch = active_.read(); patch != nullptr) {
        patch->apply_network_state(weapon, state, flag);
    } else if (const auto original = original_set_network_state_.get(); original != nullptr) {
        original(weapon, state, flag);
    }
}

void __fastcall HeroAnimationFix::hook_enter_state(void* weapon, void*, int state) noexcept {
    if (auto* patch = active_.read(); patch != nullptr) {
        patch->enter_state(weapon, state);
    } else if (const auto original = original_enter_state_.get(); original != nullptr) {
        original(weapon, state);
    }
}

void __fastcall HeroAnimationFix::hook_animator_state(void* animator, void*, std::uint32_t weapon_state,
                                                      std::uint32_t active, std::uint32_t primary_animation,
                                                      std::uint32_t secondary_animation, std::uint32_t blend) noexcept {
    auto* patch = active_.read();
    const auto suppress = patch != nullptr && patch->suppress_animator_state(animator, weapon_state, active,
                                                                             primary_animation, secondary_animation);
    if (!suppress) {
        if (const auto original = original_animator_state_.get(); original != nullptr) {
            original(animator, weapon_state, active, primary_animation, secondary_animation, blend);
        }
    }
}

void HeroAnimationFix::reconcile_input_queue(MidHookContext& context) noexcept {
    auto* patch = active_.read();
    if (patch == nullptr || gPredictionWindow.patch != patch || context.ecx < kInputQueueOffset) {
        return;
    }

    const auto weapon = context.ecx - kInputQueueOffset;
    if (gPredictionWindow.identity.weapon != weapon || !gPredictionWindow.identity.is_remote()) {
        return;
    }
    const auto buttons = read_native_field<std::uint8_t>(reinterpret_cast<const void*>(context.esp + sizeof(void*)));
    auto* queue = reinterpret_cast<void*>(context.ecx);
    auto down = read_native_field<std::uint8_t>(queue, kInputDownOffset);
    if (patch->history_.reconcile_input(gPredictionWindow.identity, buttons, down)) {
        write_native_field(queue, kInputDownOffset, down);
    }
}

void HeroAnimationFix::suppress_authority_replay(MidHookContext& context) noexcept {
    auto* patch = active_.read();
    if (patch == nullptr || gPredictionWindow.patch != patch || gPredictionWindow.identity.weapon != context.ebx) {
        return;
    }
    if (patch->history_.resolve_replay(gPredictionWindow.identity, static_cast<int>(context.esi))) {
        context.eip = patch->prediction_resume_;
    }
}

} // namespace fusioncutter::patches::hero_animation_fix
