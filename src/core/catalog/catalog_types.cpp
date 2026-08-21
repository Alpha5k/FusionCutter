#include "catalog_types.hpp"

#include "definition_copy.hpp"

#include <utility>

namespace fc::catalog {

RegistrationOwner::RegistrationOwner(RegistrationReleaseFn release) noexcept : release_(release) {}

RegistrationOwner::RegistrationOwner(RegistrationOwner&& other) noexcept
    : release_(std::exchange(other.release_, nullptr)) {}

RegistrationOwner& RegistrationOwner::operator=(RegistrationOwner&& other) noexcept {
    if (this != &other) {
        reset();
        release_ = std::exchange(other.release_, nullptr);
    }
    return *this;
}

RegistrationOwner::~RegistrationOwner() {
    reset();
}

void RegistrationOwner::reset() noexcept {
    if (release_ != nullptr) {
        release_();
        release_ = nullptr;
    }
}

Catalog::Catalog(std::vector<PluginRecord> plugins) : plugins_(std::move(plugins)) {
    // Plugin admission freezes collision results, so these dense maps cannot replace an earlier owner.
    for (std::size_t plugin_index = 0; plugin_index < plugins_.size(); ++plugin_index) {
        plugin_by_id_.emplace(fold_ascii(plugins_[plugin_index].definition.id),
                              PluginIndex{static_cast<std::uint32_t>(plugin_index)});
        for (std::size_t patch_index = 0; patch_index < plugins_[plugin_index].definition.patches.size();
             ++patch_index) {
            const auto index = PatchIndex{static_cast<std::uint32_t>(patches_.size())};
            patches_.push_back({static_cast<std::uint32_t>(plugin_index), static_cast<std::uint32_t>(patch_index)});
            patch_by_id_.emplace(fold_ascii(plugins_[plugin_index].definition.patches[patch_index].id), index);
        }
        for (std::size_t group_index = 0; group_index < plugins_[plugin_index].definition.groups.size();
             ++group_index) {
            const auto index = GroupIndex{static_cast<std::uint32_t>(groups_.size())};
            groups_.push_back({static_cast<std::uint32_t>(plugin_index), static_cast<std::uint32_t>(group_index)});
            group_by_id_.emplace(fold_ascii(plugins_[plugin_index].definition.groups[group_index].id), index);
        }
    }
}

std::span<const PluginRecord> Catalog::plugins() const noexcept {
    return plugins_;
}

std::size_t Catalog::patch_count() const noexcept {
    return patches_.size();
}

std::size_t Catalog::group_count() const noexcept {
    return groups_.size();
}

std::optional<PluginIndex> Catalog::find_plugin(std::string_view id) const {
    const auto found = plugin_by_id_.find(fold_ascii(id));
    return found == plugin_by_id_.end() ? std::nullopt : std::optional{found->second};
}

std::optional<PatchIndex> Catalog::find_patch(std::string_view id) const {
    const auto found = patch_by_id_.find(fold_ascii(id));
    return found == patch_by_id_.end() ? std::nullopt : std::optional{found->second};
}

std::optional<GroupIndex> Catalog::find_group(std::string_view id) const {
    const auto found = group_by_id_.find(fold_ascii(id));
    return found == group_by_id_.end() ? std::nullopt : std::optional{found->second};
}

const PatchDefinitionRecord& Catalog::patch(PatchIndex index) const noexcept {
    const auto location = patches_[index.value];
    return plugins_[location.plugin].definition.patches[location.patch];
}

const GroupDefinitionRecord& Catalog::group(GroupIndex index) const noexcept {
    const auto location = groups_[index.value];
    return plugins_[location.plugin].definition.groups[location.group];
}

PluginIndex Catalog::patch_plugin(PatchIndex index) const noexcept {
    return PluginIndex{patches_[index.value].plugin};
}

std::uint32_t Catalog::patch_ordinal(PatchIndex index) const noexcept {
    return patches_[index.value].patch;
}

PluginIndex Catalog::group_plugin(GroupIndex index) const noexcept {
    return PluginIndex{groups_[index.value].plugin};
}

std::uint32_t Catalog::group_ordinal(GroupIndex index) const noexcept {
    return groups_[index.value].group;
}

const PluginRecord& Catalog::plugin(PluginIndex index) const noexcept {
    return plugins_[index.value];
}

} // namespace fc::catalog
