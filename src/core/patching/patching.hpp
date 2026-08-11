#pragma once

#include "FusionCutter/outcome.hpp"
#include "FusionCutter/patching.hpp"

#include <expected>
#include <memory>
#include <string_view>

namespace fusioncutter {

struct CommitFailure {
    OutcomeReason reason;
    bool rollback_failed;
};

class MutationReservations;

class PreparedPatchPlan {
  public:
    class Impl;

    PreparedPatchPlan(const PreparedPatchPlan&) = delete;
    PreparedPatchPlan(PreparedPatchPlan&&) noexcept;
    PreparedPatchPlan& operator=(const PreparedPatchPlan&) = delete;
    PreparedPatchPlan& operator=(PreparedPatchPlan&&) noexcept;
    ~PreparedPatchPlan();

    [[nodiscard]] static std::expected<PreparedPatchPlan, OutcomeReason> prepare(PatchPlan&& plan);

    [[nodiscard]] PatchId patch_id() const noexcept;
    [[nodiscard]] std::expected<void, CommitFailure> commit();
    [[nodiscard]] std::expected<void, OutcomeReason> rollback();

  private:
    explicit PreparedPatchPlan(std::unique_ptr<Impl> impl) noexcept;

    std::unique_ptr<Impl> impl_;

    friend class MutationReservations;
};

class MutationReservations {
  public:
    MutationReservations();
    MutationReservations(const MutationReservations&) = delete;
    MutationReservations(MutationReservations&&) noexcept;
    MutationReservations& operator=(const MutationReservations&) = delete;
    MutationReservations& operator=(MutationReservations&&) noexcept;
    ~MutationReservations();

    [[nodiscard]] std::expected<void, OutcomeReason> reserve(PreparedPatchPlan& plan);

  private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace fusioncutter
