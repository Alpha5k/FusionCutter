#pragma once

#include <fc/plugin/declarations.hpp>
#include <fc/base.hpp>
#include <fc/detail/handler_adapter.hpp>
#include <cstdint>
#include <exception>
#include <span>
#include <stdexcept>
#include <string>
#include <typeindex>
#include <utility>
#include <vector>

namespace fc::detail {

// Owns the selector array referenced by one image requirement.
struct ImageStorage {
    std::vector<FC_Text> selectors;
};

// Owns the image and dependency arrays referenced by one lowered FC_Variant.
struct VariantStorage {
    std::vector<ImageStorage> image_storage;
    std::vector<FC_ImageRequirement> images;
    std::vector<FC_Text> dependencies;
};

// Owns the variant and dependency arrays referenced by one lowered FC_Patch.
struct PatchStorage {
    std::vector<VariantStorage> variant_storage;
    std::vector<FC_Variant> variants;
    std::vector<FC_Text> dependencies;
};

// Owns member views referenced by one lowered group.
struct GroupStorage {
    std::vector<FC_Text> members;
};

// Owns one C++ plugin declaration and the C records built from it.
// Their internal pointers remain valid while this object stays alive at a fixed address.
class PluginStorage {
  public:
    // Lowers an owned plugin declaration; missing defaults or mismatched settings types throw.
    explicit PluginStorage(Plugin plugin) : plugin_(std::move(plugin)) {
        // Build nested C records first so the plugin descriptor can point at stable arrays.
        lower_categories();
        lower_groups();
        lower_patches();

        descriptor_ = {
            .byte_size = sizeof(FC_Plugin),
            .id = text(plugin_.id),
            .version = text(plugin_.version),
            .categories = categories_.empty() ? nullptr : categories_.data(),
            .category_count = count(categories_.size()),
            .groups = groups_.empty() ? nullptr : groups_.data(),
            .group_count = count(groups_.size()),
            .patches = patches_.empty() ? nullptr : patches_.data(),
            .patch_count = count(patches_.size()),
        };
    }

    // Nested C records point into this object, so its address must remain fixed after construction.
    PluginStorage(const PluginStorage &) = delete;
    PluginStorage &operator=(const PluginStorage &) = delete;
    PluginStorage(PluginStorage &&) = delete;
    PluginStorage &operator=(PluginStorage &&) = delete;

    // Returns this object's C declaration; the storage must outlive all uses of its nested views.
    const FC_Plugin *descriptor() const noexcept {
        return &descriptor_;
    }

  private:
    // Builds the C category records from the owned declarations.
    void lower_categories() {
        // Preserve explicit zero order separately from an omitted category order.
        categories_.reserve(plugin_.categories.size());
        for (const auto &category : plugin_.categories) {
            categories_.push_back(
                {.id = text(category.id), .has_order = category.order.has_value() ? 1u : 0u, .order = category.order.value_or(0)});
        }
    }

    // Builds C group records and their member arrays; an omitted enabled default throws.
    void lower_groups() {
        group_storage_.reserve(plugin_.groups.size());
        groups_.reserve(plugin_.groups.size());
        for (const auto &group : plugin_.groups) {
            if (!group.enabled.has_value()) {
                throw std::invalid_argument{"group '" + group.id + "' must declare enabled"};
            }

            auto &lowered = group_storage_.emplace_back();
            // Finish each member array before saving its pointer in the group record.
            lowered.members.reserve(group.members.size());
            for (const auto &member : group.members) {
                lowered.members.push_back(text(member));
            }

            groups_.push_back({
                .id = text(group.id),
                .description = text(group.description),
                .category = text(group.category),
                .members = lowered.members.empty() ? nullptr : lowered.members.data(),
                .member_count = count(lowered.members.size()),
                .enabled = *group.enabled ? 1u : 0u,
                .configurable = group.configurable ? 1u : 0u,
            });
        }
    }

    // Builds C patches, variants and factories, resolving schemas and policies and checking handler settings types.
    void lower_patches() {
        // Reserve every owning level before its addresses are copied into C records.
        patch_storage_.reserve(plugin_.patches.size());
        patches_.reserve(plugin_.patches.size());
        for (auto &patch : plugin_.patches) {
            if (!patch.enabled.has_value()) {
                throw std::invalid_argument{"patch '" + patch.id + "' must declare enabled"};
            }

            auto &lowered = patch_storage_.emplace_back();
            lowered.dependencies.reserve(patch.depends_on.size());
            for (const auto &dependency : patch.depends_on) {
                lowered.dependencies.push_back(text(dependency));
            }

            // variant_storage keeps each variant's image and dependency arrays alive.
            // variants holds the C structs that point to those arrays.
            lowered.variant_storage.reserve(patch.variants.size());
            lowered.variants.reserve(patch.variants.size());
            for (auto &source_variant : patch.variants) {
                // A variant replaces the complete schema when explicit; otherwise it inherits the patch schema.
                if (!source_variant.settings_) {
                    source_variant.settings_ = patch.settings;
                }

                // Match the effective schema's C++ type to the handler before exposing its factory.
                auto &settings = *source_variant.settings_;
                const auto model = settings.model;
                if ((model ? model->type() : std::type_index{typeid(void)}) != source_variant.settings_type_) {
                    throw std::invalid_argument("Handler settings type does not match its effective schema");
                }

                const auto schema = model ? model->records() : std::span<const FC_Setting>{};
                auto &variant = lowered.variant_storage.emplace_back();
                variant.dependencies.reserve(source_variant.depends_on_.size());
                for (const auto &dependency : source_variant.depends_on_) {
                    variant.dependencies.push_back(text(dependency));
                }

                // Each required image owns an array of alternative profile selectors.
                variant.image_storage.reserve(source_variant.images_.size());
                variant.images.reserve(source_variant.images_.size());
                for (const auto &source_image : source_variant.images_) {
                    auto &image = variant.image_storage.emplace_back();
                    image.selectors.reserve(source_image.profiles.size());
                    for (const auto &selector : source_image.profiles) {
                        image.selectors.push_back(text(selector));
                    }

                    variant.images.push_back({
                        .name = text(source_image.name),
                        .selectors = image.selectors.empty() ? nullptr : image.selectors.data(),
                        .selector_count = count(image.selectors.size()),
                    });
                }

                // Fill the image and dependency arrays before storing their addresses in the FC_Variant.
                lowered.variants.push_back({
                    .images = variant.images.empty() ? nullptr : variant.images.data(),
                    .image_count = count(variant.images.size()),
                    .role = static_cast<std::uint32_t>(source_variant.role_),
                    .failure_policy = static_cast<std::uint32_t>(source_variant.failure_policy_.value_or(patch.failure_policy)),
                    .depends_on = variant.dependencies.empty() ? nullptr : variant.dependencies.data(),
                    .dependency_count = count(variant.dependencies.size()),
                    .settings = schema.data(),
                    .setting_count = count(schema.size()),
                    .factory = {&settings, source_variant.create_},
                });
            }

            patches_.push_back({
                .id = text(patch.id),
                .description = text(patch.description),
                .category = text(patch.category),
                .enabled = *patch.enabled ? 1u : 0u,
                .configurable = patch.configurable ? 1u : 0u,
                .failure_policy = static_cast<std::uint32_t>(patch.failure_policy),
                .update_interval_ms = patch.update_interval.count(),
                .depends_on = lowered.dependencies.empty() ? nullptr : lowered.dependencies.data(),
                .dependency_count = count(lowered.dependencies.size()),
                .variants = lowered.variants.empty() ? nullptr : lowered.variants.data(),
                .variant_count = count(lowered.variants.size()),
            });
        }
    }

    // Owns the strings referenced by the C records.
    Plugin plugin_;
    std::vector<FC_Category> categories_;
    std::vector<GroupStorage> group_storage_;
    std::vector<FC_Group> groups_;
    std::vector<PatchStorage> patch_storage_;
    std::vector<FC_Patch> patches_;

    // Plugin C record returned to the runtime.
    FC_Plugin descriptor_{};
};

// Returns 1 with stable declarations for FC_ABI_VERSION, or 0 with an optional synchronous error and supplied out cleared.
template <auto factory> std::uint32_t get_plugin(std::uint32_t abi, const FC_Plugin **out, FC_ErrorSink error) noexcept {
    if (out == nullptr) {
        write_error(error, "plugin output pointer is null");
        return 0;
    }

    *out = nullptr;
    if (abi != FC_ABI_VERSION) {
        write_error(error, "requested plugin ABI " + std::to_string(abi) + "; this plugin provides ABI " + std::to_string(FC_ABI_VERSION));
        return 0;
    }

    try {
        // Other threads wait for initialization; if it throws, a later call retries.
        // Keep successful storage until process exit because the runtime holds pointers into it.
        static auto *storage = new PluginStorage(factory());
        *out = storage->descriptor();
        return 1;
    } catch (const std::exception &exception) {
        write_error(error, exception.what());
    } catch (...) {
        write_error(error, "plugin declaration construction failed with an unknown exception");
    }
    return 0;
}

} // namespace fc::detail
