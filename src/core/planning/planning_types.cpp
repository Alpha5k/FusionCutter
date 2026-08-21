#include "planning_types.hpp"

#include "../fatal_boundary.hpp"

#include <cassert>
#include <exception>
#include <utility>

namespace fc::planning {

void ResolvedSettings::push(config::ResolvedSettingValue value) {
    values_.push_back(std::move(value));
}

std::size_t ResolvedSettings::size() const noexcept {
    return values_.size();
}

std::vector<FC_SettingValue> ResolvedSettings::native_values() const {
    // Materialize a flat callback-scoped view while each owning ResolvedSettingValue keeps string storage stable.
    std::vector<FC_SettingValue> result;
    result.reserve(values_.size());
    for (const auto& value : values_) {
        result.push_back(value.native_value());
    }
    return result;
}

PatchInstance::PatchInstance(const FC_PatchCallbacks& callbacks, FC_PatchHandle handle) noexcept
    : callbacks_(&callbacks), handle_(handle) {}

PatchInstance::PatchInstance(PatchInstance&& other) noexcept
    : callbacks_(std::exchange(other.callbacks_, nullptr)), handle_(std::exchange(other.handle_, nullptr)) {}

PatchInstance& PatchInstance::operator=(PatchInstance&& other) noexcept {
    if (this != &other) {
        reset();
        callbacks_ = std::exchange(other.callbacks_, nullptr);
        handle_ = std::exchange(other.handle_, nullptr);
    }
    return *this;
}

PatchInstance::~PatchInstance() {
    reset();
}

FC_PatchHandle PatchInstance::get() const noexcept {
    return handle_;
}

const FC_PatchCallbacks& PatchInstance::callbacks() const noexcept {
    assert(callbacks_ != nullptr);
    return *callbacks_;
}

bool PatchInstance::has_update() const noexcept {
    return callbacks_ != nullptr && callbacks_->update != nullptr;
}

void PatchInstance::update(FC_ReportToken report) noexcept {
    if (!has_update()) {
        return;
    }
    const FC_UpdateContext context{.struct_size = sizeof(FC_UpdateContext), .report = report};
    try {
        callbacks_->update(callbacks_->context, handle_, &context);
    } catch (...) {
        // The Update callback is nonthrowing; an exception across this boundary compromises process integrity.
        fatal_invariant("An installed patch Update callback threw across the native boundary");
    }
}

void PatchInstance::reset() noexcept {
    if (callbacks_ != nullptr && handle_ != nullptr) {
        // Native destroy callbacks cannot unwind through the framework, including from an RAII cleanup path.
        try {
            callbacks_->destroy(callbacks_->context, handle_);
        } catch (...) {
            // Destruction has no reporting channel; containment preserves the noexcept ownership boundary.
        }
    }
    callbacks_ = nullptr;
    handle_ = nullptr;
}

PatchWorkSet::PatchWorkSet(const catalog::Catalog& catalog) : catalog_(&catalog) {
    records_.reserve(catalog.patch_count());
    for (std::size_t index = 0; index < catalog.patch_count(); ++index) {
        records_.push_back(PatchWorkRecord{.patch = catalog::PatchIndex{static_cast<std::uint32_t>(index)},
                                           .state = PatchState::Pending});
    }
}

const catalog::Catalog& PatchWorkSet::catalog() const noexcept {
    return *catalog_;
}

std::span<PatchWorkRecord> PatchWorkSet::records() noexcept {
    return records_;
}

std::span<const PatchWorkRecord> PatchWorkSet::records() const noexcept {
    return records_;
}

PatchWorkRecord& PatchWorkSet::record(catalog::PatchIndex patch) noexcept {
    return records_[patch.value];
}

const PatchWorkRecord& PatchWorkSet::record(catalog::PatchIndex patch) const noexcept {
    return records_[patch.value];
}

void finish_inactive_patch(PatchWorkRecord& record, PatchState state, FailureReason reason) {
    assert(state == PatchState::Skipped || state == PatchState::Failed);
    // Destroy plugin state first because its destructors may depend on callbacks and values owned by the patch plan.
    record.instance = {};
    record.settings = {};
    record.plan = {};
    record.state = state;
    record.reason = std::move(reason);
}

} // namespace fc::planning
