#include "plan.hpp"

#include "patches/patch_record.hpp"
#include "patches/selection.hpp"
#include "hook_registry.hpp"

#include <Windows.h>

#include <algorithm>
#include <cstring>
#include <limits>

namespace fc::runtime {

namespace {

// Adapts operation submission to the C boundary and reports failures through the caller's sink.
std::uint32_t FC_CDECL submit_callback(void *context, const FC_Operation *operation, void **out, FC_ErrorSink sink) {
    if (out) {
        *out = nullptr;
    }

    auto &plan = *static_cast<SubmittedPlan *>(context);
    if (!operation) {
        plan.fail({"Native operation pointer is null"});
        detail::report(sink, *plan.error());
        return 0;
    }

    auto result = plan.submit(*operation, out);
    if (!result) {
        detail::report(sink, result.error());
    }
    return result ? 1u : 0u;
}

// Adapts snapshot reads to the C boundary while retaining invalid requests as plan failures.
std::uint32_t FC_CDECL read_callback(void *context, const FC_ReadClaim *claim, void *out, FC_ErrorSink sink) {
    auto &plan = *static_cast<SubmittedPlan *>(context);
    if (!claim) {
        plan.fail({"Native snapshot request is null"});
        detail::report(sink, *plan.error());
        return 0;
    }

    auto result = plan.read(*claim, out);
    if (!result) {
        detail::report(sink, result.error());
    }
    return result ? 1u : 0u;
}

} // namespace

std::expected<std::vector<PlanImage>, Diagnostic> selected_plan_images(const PatchRecord &patch,
                                                                       std::span<const LiveImageObservation> observed) {
    std::vector<PlanImage> result;
    if (patch.selection.state != SelectionState::Ready) {
        return std::unexpected(Diagnostic{patch.resolved.owner, "Native plan requires a ready image selection"});
    }

    // Preserve selection order so the exposed labels describe the same pinned mappings.
    for (const auto &binding : patch.selection.images) {
        if (binding.observation_index >= observed.size() || !observed[binding.observation_index].outcome ||
            !*observed[binding.observation_index].outcome) {
            return std::unexpected(Diagnostic{patch.resolved.owner, "Selected image has no pinned live mapping"});
        }
        const auto &image = **observed[binding.observation_index].outcome;
        result.push_back({std::string{binding.label}, std::string{binding.profile->id}, image.base, image.image_size, {}});
    }

    return result;
}

std::expected<std::vector<std::byte>, Diagnostic> map_file_image(const FileImage &image, std::span<const std::byte> file) {
    if (!image.layout.preferred_base || image.layout.preferred_base > std::numeric_limits<std::uintptr_t>::max() ||
        image.layout.image_size > std::numeric_limits<std::uintptr_t>::max() - image.layout.preferred_base) {
        return std::unexpected(
            Diagnostic{image.filename, "Offline preferred image range has a null base or exceeds the native address space"});
    }

    // Zero-filled storage includes virtual section tails beyond the bytes present in the file.
    std::vector<std::byte> mapped(image.layout.image_size);
    for (const auto &section : image.layout.sections) {
        if (section.raw_size && (section.raw_offset > file.size() || section.raw_size > file.size() - section.raw_offset)) {
            return std::unexpected(Diagnostic{image.filename, "Offline section bytes are outside the file"});
        }

        // PE parsing has already bounded section RVAs; copy only the portion occupying mapped memory.
        const auto count = std::min(section.raw_size, section.mapped_size);
        if (count) {
            std::memcpy(mapped.data() + section.rva, file.data() + section.raw_offset, count);
        }
    }

    return mapped;
}

PlanImage mapped_plan_image(std::string name, const FileImage &image, std::span<const std::byte> mapped) {
    PlanImage result{std::move(name),
                     std::string{image.profile->id},
                     static_cast<std::uintptr_t>(image.layout.preferred_base),
                     image.layout.image_size,
                     {},
                     mapped};

    // Express offline permission ranges in the preferred address space used by evidence checks.
    for (const auto &section : image.layout.sections) {
        result.sections.push_back({section.name, result.base + section.rva, section.mapped_size, section.characteristics});
    }

    return result;
}

SubmittedPlan::SubmittedPlan(PatchRecord *owner, std::vector<PlanImage> images, HookRegistry *hooks)
    : owner_(owner), hooks_(hooks), images_(std::move(images)) {
    // Image strings remain owned by this plan while the ABI label views borrow their storage.
    bindings_.reserve(images_.size());
    for (const auto &image : images_) {
        bindings_.push_back({detail::text(image.name), detail::text(image.profile)});
    }

    // Find the handler factory's module so callbacks and function targets can be checked against it.
    if (owner_ && (owner_->internal_factory.create || owner_->selection.variant)) {
        const auto create = owner_->internal_factory.create ? owner_->internal_factory.create : owner_->selection.variant->factory.create;
        const auto executable = patching::check_native_access(reinterpret_cast<std::uintptr_t>(create), 1, false, true);
        if (!executable) {
            fail({"Could not verify executable access to the handler factory"});
        }

        HMODULE module{};
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCWSTR>(create), &module)) {
            plugin_module_ = module;
        }
    }
}

SubmittedPlan::~SubmittedPlan() = default;

FC_Plan SubmittedPlan::boundary() noexcept {
    return {this, &submit_callback, &read_callback, bindings_.data(), static_cast<std::uint32_t>(bindings_.size())};
}

void SubmittedPlan::fail(Error error) {
    if (!error_) {
        error_ = std::move(error);
    }
}

std::string SubmittedPlan::owner_id() const {
    if (owner_ && owner_->plugin && owner_->plugin->admitted.definition && owner_->definition) {
        return qualified_patch_id(*owner_);
    }
    return owner_ ? owner_->resolved.owner : std::string{"native"};
}

} // namespace fc::runtime
