#include "dlc_mission_limit.hpp"

#include "layout.hpp"

#include <cstddef>
#include <cstdint>

namespace fusioncutter::patches::dlc_mission_limit {
namespace {

constexpr std::size_t kMissionRecordSize = 0x110;
constexpr std::uint32_t kOriginalMissionLimit = 500;
constexpr std::uint32_t kExpandedMissionLimit = 4096;

} // namespace

DLCMissionLimit::DLCMissionLimit(const TargetContext& target) noexcept : layout_(target.layout), image_(target.image) {}

void DLCMissionLimit::build_plan(PatchPlan& plan) {
    // The game keeps 500 fixed-size DLC mission records in its image; the expanded table needs new storage.
    auto mission_table =
        plan.allocate_data<std::byte>("Allocate DLC mission storage", kMissionRecordSize * kExpandedMissionLimit);

    plan.checked_write("Raise DLC mission limit", limit_rva(layout_), kOriginalMissionLimit, kExpandedMissionLimit);

    // Native code addresses fields in the first record, so preserve each field offset in the replacement table.
    const auto legacy_table = image_.address_at_rva(legacy_table_rva(layout_));
    for (const auto& site : kMissionPointerSites) {
        const auto expected = legacy_table + site.table_offset;
        plan.checked_write(site.operation, site_rva(site, layout_), exact_pattern(expected),
                           mission_table.offset(site.table_offset));
    }
}

} // namespace fusioncutter::patches::dlc_mission_limit
