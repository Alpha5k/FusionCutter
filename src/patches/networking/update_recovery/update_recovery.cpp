#include "update_recovery.hpp"

#include "ordering.hpp"

#include <cstddef>

namespace fusioncutter::patches::update_recovery {
namespace {

constexpr std::size_t kPipeIndexFrameOffset = 0x14;
constexpr std::size_t kReceiveTimestampFrameOffset = 0x10;
constexpr int kCompleteUpdatePacketType = 0x0C;

} // namespace

LateUpdateRecovery::LateUpdateRecovery(const TargetContext& target) noexcept
    : layout_(layout_for(target.layout)), image_(target.image),
      construct_group_(image_.function_at_rva<ConstructGroup>(layout_.functions.construct_group.rva)),
      receive_group_(image_.function_at_rva<ReceiveGroup>(layout_.functions.receive_group.rva)),
      get_update_turn_(image_.function_at_rva<GetUpdateTurn>(layout_.functions.get_update_turn.rva)),
      receive_update_(image_.function_at_rva<ReceiveUpdate>(layout_.functions.receive_update.rva)),
      get_last_receive_(image_.function_at_rva<GetLastReceive>(layout_.functions.get_last_receive.rva)),
      host_cycle_timer_(image_.mutable_at_rva<float>(layout_.state.host_cycle_timer_rva)),
      host_cycle_extension_(image_.read_at_rva<float>(layout_.state.host_cycle_extension_rva)),
      resume_address_(image_.address_at_rva(layout_.frame.resume.rva)) {}

void LateUpdateRecovery::build_plan(PatchPlan& plan) {
    add_layout_requirements(plan, image_, layout_);
    const auto drain = drain_preimage(image_, layout_);
    plan.mid_hook("Recover complete network updates", layout_.frame.drain.rva, BytePattern::exact(drain),
                  &LateUpdateRecovery::recover_complete_updates);
}

void LateUpdateRecovery::enable_runtime() noexcept {
    active_.publish(*this);
}

void LateUpdateRecovery::disable_runtime() noexcept {
    active_.clear(*this);
}

void LateUpdateRecovery::recover_complete_updates(MidHookContext& context) noexcept {
    auto* patch = active_.read();
    if (patch == nullptr || context.ebp == 0) {
        return;
    }

    const auto* frame = reinterpret_cast<const std::byte*>(context.ebp);
    const auto pipe_index_base = read_native_field<int>(frame - kPipeIndexFrameOffset);
    const auto receive_timestamp = read_native_field<float>(frame - kReceiveTimestampFrameOffset);

    patch->drain(receive_timestamp, pipe_index_base);
    context.eip = patch->resume_address_;
}

void LateUpdateRecovery::drain(float receive_timestamp, int pipe_index_base) noexcept {
    for (;;) {
        std::size_t count{};
        for (; count < updates_.size(); ++count) {
            auto& update = updates_[count];
            static_cast<void>(construct_group_(update.group.data()));
            if (!receive_group_(update.group.data(), kCompleteUpdatePacketType)) {
                break;
            }
            update.turn = get_update_turn_(update.group.data());
        }

        if (count == 0) {
            return;
        }

        auto batch = std::span{updates_}.first(count);
        order_updates_by_turn(batch);
        for (auto& update : batch) {
            receive_update_(update.group.data());
            refresh_receive_timers(receive_timestamp, pipe_index_base);
        }

        if (count < updates_.size()) {
            return;
        }
    }
}

void LateUpdateRecovery::refresh_receive_timers(float receive_timestamp, int pipe_index_base) const noexcept {
    constexpr int kPipeReceiveStateOffset = 0x40;
    *get_last_receive_(pipe_index_base + kPipeReceiveStateOffset) = receive_timestamp;
    *host_cycle_timer_ = receive_timestamp + *host_cycle_extension_;
}

} // namespace fusioncutter::patches::update_recovery
