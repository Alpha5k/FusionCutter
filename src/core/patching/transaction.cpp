#include "prepared_plan.hpp"

#include "validation.hpp"

#include <expected>
#include <iterator>
#include <memory>
#include <optional>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace fusioncutter {
using namespace patching_detail;

namespace {

[[nodiscard]] bool overlaps(const Claim& left, const Claim& right) noexcept {
    return left.begin < right.end && right.begin < left.end;
}

} // namespace

class MutationReservations::Impl {
  public:
    std::vector<Claim> claims;
};

std::expected<void, CommitFailure> PreparedPatchPlan::commit() {
    if (!impl_ || impl_->state != TransactionState::Prepared) {
        return std::unexpected(CommitFailure{operation_failure("patch plan is not ready to commit"), false});
    }
    if (!impl_->ranges_reserved) {
        return std::unexpected(CommitFailure{operation_failure("patch plan has no mutation-range reservation"), false});
    }
    if (auto revalidated = impl_->revalidate(); !revalidated) {
        impl_->release_resources();
        impl_->state = TransactionState::RolledBack;
        return std::unexpected(CommitFailure{revalidated.error(), false});
    }

    std::optional<OutcomeReason> commit_failure;
    for (auto& operation : impl_->operations) {
        auto result = std::visit(
            [&](auto& state) -> std::expected<void, OutcomeReason> {
                using State = std::remove_cvref_t<decltype(state)>;
                if constexpr (std::same_as<State, PreparedRequirement>) {
                    return {};
                } else {
                    if constexpr (std::same_as<State, PreparedInlineHook>) {
                        state.original_slot->store(state.hook.trampoline().address(), std::memory_order_release);
                        if (auto enabled = state.hook.enable(); !enabled) {
                            state.original_slot->store(0, std::memory_order_release);
                            return std::unexpected(
                                operation_failure(inline_hook_error(enabled.error()), operation.name));
                        }
                        state.committed = true;
                    } else if constexpr (std::same_as<State, PreparedMidHook>) {
                        if (auto enabled = state.hook.enable(); !enabled) {
                            return std::unexpected(operation_failure(mid_hook_error(enabled.error()), operation.name));
                        }
                        state.committed = true;
                    } else {
                        if constexpr (std::same_as<State, PreparedRedirect>) {
                            if (state.original_slot) {
                                state.original_slot->store(state.original_destination, std::memory_order_release);
                            }
                        }
                        auto written = write_memory(state.target, state.replacement);
                        if (!written) {
                            state.committed = written.error().memory_changed;
                            if constexpr (std::same_as<State, PreparedRedirect>) {
                                if (!state.committed && state.original_slot) {
                                    state.original_slot->store(0, std::memory_order_release);
                                }
                            }
                            return std::unexpected(operation_failure(written.error().message, operation.name));
                        }
                        state.committed = true;
                    }
                    return {};
                }
            },
            operation.state);
        if (!result) {
            commit_failure = std::move(result.error());
            break;
        }
    }

    if (!commit_failure) {
        impl_->state = TransactionState::Committed;
        return {};
    }
    if (auto rolled_back = impl_->rollback_operations(); !rolled_back) {
        return std::unexpected(CommitFailure{rolled_back.error(), true});
    }
    impl_->release_resources();
    return std::unexpected(CommitFailure{std::move(*commit_failure), false});
}

std::expected<void, OutcomeReason> PreparedPatchPlan::rollback() {
    if (!impl_ || impl_->state != TransactionState::Committed) {
        return std::unexpected(operation_failure("patch plan is not committed"));
    }
    if (auto result = impl_->rollback_operations(); !result) {
        return result;
    }
    impl_->release_resources();
    return {};
}

MutationReservations::MutationReservations() : impl_(std::make_unique<Impl>()) {}
MutationReservations::MutationReservations(MutationReservations&&) noexcept = default;
MutationReservations& MutationReservations::operator=(MutationReservations&&) noexcept = default;
MutationReservations::~MutationReservations() = default;

std::expected<void, OutcomeReason> MutationReservations::reserve(PreparedPatchPlan& plan) {
    if (!plan.impl_ || plan.impl_->state != TransactionState::Prepared || plan.impl_->ranges_reserved) {
        return std::unexpected(operation_failure("patch plan is not ready for range reservation"));
    }
    auto pending = plan.impl_->claims();

    for (std::size_t left = 0; left < pending.size(); ++left) {
        for (std::size_t right = left + 1; right < pending.size(); ++right) {
            if (pending[left].operation_index == pending[right].operation_index ||
                (!pending[left].mutation && !pending[right].mutation) || !overlaps(pending[left], pending[right])) {
                continue;
            }
            auto reason =
                operation_failure("patch operations claim overlapping target bytes", pending[right].operation);
            reason.related_patch = plan.impl_->patch_id;
            return std::unexpected(std::move(reason));
        }
    }

    for (const auto& claim : pending) {
        for (const auto& existing : impl_->claims) {
            if ((!claim.mutation && !existing.mutation) || !overlaps(claim, existing)) {
                continue;
            }
            auto reason = operation_failure("target bytes are already claimed by another patch", claim.operation);
            reason.related_patch = existing.patch_id;
            return std::unexpected(std::move(reason));
        }
    }

    impl_->claims.insert(impl_->claims.end(), std::make_move_iterator(pending.begin()),
                         std::make_move_iterator(pending.end()));
    plan.impl_->ranges_reserved = true;
    return {};
}

} // namespace fusioncutter
