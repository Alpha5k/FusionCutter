#include "evidence_validation.hpp"

#include "native_address.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <ranges>
#include <span>
#include <vector>

namespace fc::planning {
namespace {

[[nodiscard]] bool checked_extent(std::uint32_t rva, std::uint64_t size, std::size_t image_size) noexcept {
    return size != 0 && rva <= image_size && size <= image_size - rva;
}

// Reads a reviewed image range and can reconstruct the logical view before the hook for late participants.
[[nodiscard]] std::expected<std::vector<std::byte>, std::string>
read_image(const targets::ImageView& image, std::uint32_t rva, std::uint64_t size, std::string_view purpose,
           std::optional<FC_TargetImage> image_id = std::nullopt,
           std::span<const InstalledHookSite> installed_hooks = {}) {
    if (!checked_extent(rva, size, image.info().size) || !image.is_readable({rva}, static_cast<std::size_t>(size))) {
        return std::unexpected(std::string{purpose} + " is outside reviewed readable image bounds");
    }
    std::vector<std::byte> observed(static_cast<std::size_t>(size));
    if (!image.read({rva}, observed)) {
        return std::unexpected(std::string{purpose} + " could not be read from the target image");
    }

    // Installed hooks intentionally changed the live image. Overlay their retained preimages so a later patch is
    // validated against the same logical original code that the first participant saw.
    if (image_id) {
        const auto read_begin = static_cast<std::uint64_t>(rva);
        const auto read_end = read_begin + size;
        for (const auto& site : installed_hooks) {
            if (site.image != *image_id || site.original_bytes.empty()) {
                continue;
            }
            const auto site_begin = static_cast<std::uint64_t>(site.rva);
            const auto site_end = site_begin + site.original_bytes.size();
            const auto overlap_begin = std::max(read_begin, site_begin);
            const auto overlap_end = std::min(read_end, site_end);
            if (overlap_begin >= overlap_end) {
                continue;
            }
            const auto destination = static_cast<std::size_t>(overlap_begin - read_begin);
            const auto source = static_cast<std::size_t>(overlap_begin - site_begin);
            const auto count = static_cast<std::size_t>(overlap_end - overlap_begin);
            std::ranges::copy(site.original_bytes.subspan(source, count), observed.begin() + destination);
        }
    }
    return observed;
}

// Decodes one already-read rel32 instruction so evidence about the logical Original does not reread patched live bytes.
[[nodiscard]] std::expected<std::uintptr_t, std::string> decode_direct_branch(std::span<const std::byte> instruction,
                                                                              std::uintptr_t next_instruction,
                                                                              FC_Architecture architecture,
                                                                              FC_RedirectKind kind) {
    const auto opcode = kind == FC_REDIRECT_CALL ? std::byte{0xe8} : std::byte{0xe9};
    if (instruction.size() != 5 || instruction.front() != opcode) {
        return std::unexpected("Direct branch instruction no longer has its required form");
    }
    std::int32_t displacement{};
    std::memcpy(&displacement, instruction.data() + 1, sizeof(displacement));
    const auto target = decode_rel32_target(next_instruction, displacement, architecture);
    if (!target) {
        return std::unexpected("Direct branch target overflows the native address domain");
    }
    return *target;
}

} // namespace

std::uint64_t evidence_extent(const EvidenceRecord& evidence, FC_Architecture architecture) noexcept {
    if (evidence.kind == FC_EVIDENCE_EXACT_BYTES || evidence.kind == FC_EVIDENCE_MASKED_BYTES) {
        return evidence.bytes.size();
    }
    if (evidence.kind == FC_EVIDENCE_POINTS_TO) {
        return architecture == FC_ARCH_X86 ? 4U : 8U;
    }
    if (evidence.kind == FC_EVIDENCE_DIRECT_CALL_TO || evidence.kind == FC_EVIDENCE_DIRECT_JUMP_TO) {
        return 5;
    }
    return 0;
}

std::expected<void, std::string> validate_location_evidence(const targets::ImageView& image,
                                                            FC_Architecture architecture,
                                                            const LocationRecord& location) {
    return validate_location_evidence(image, architecture, location, {}, {});
}

std::expected<void, std::string> validate_location_evidence(const targets::ImageView& image,
                                                            FC_Architecture architecture,
                                                            const LocationRecord& location, FC_TargetImage image_id,
                                                            std::span<const InstalledHookSite> installed_hooks) {
    const auto size = evidence_extent(location.evidence, architecture);
    if (size == 0) {
        return {};
    }
    auto observed = read_image(image, location.rva, size, "Evidence range", image_id, installed_hooks);
    if (!observed) {
        return std::unexpected(observed.error());
    }

    const auto& evidence = location.evidence;
    // Byte evidence verifies either the entire preimage or only the bits selected by its mask.
    if (evidence.kind == FC_EVIDENCE_EXACT_BYTES && *observed != evidence.bytes) {
        return std::unexpected("Exact byte evidence does not match the target image");
    }
    if (evidence.kind == FC_EVIDENCE_MASKED_BYTES) {
        for (std::size_t index = 0; index < observed->size(); ++index) {
            if (((*observed)[index] & evidence.mask[index]) != (evidence.bytes[index] & evidence.mask[index])) {
                return std::unexpected("Masked byte evidence does not match the target image");
            }
        }
    }
    // Relational evidence compares decoded native meaning rather than assumptions about host byte patterns.
    if (evidence.kind == FC_EVIDENCE_POINTS_TO) {
        if (evidence.target_rva >= image.info().size) {
            return std::unexpected("Pointer evidence target is outside the selected image");
        }
        std::uintptr_t actual{};
        std::memcpy(&actual, observed->data(), observed->size());
        if (actual != image.info().base + evidence.target_rva) {
            return std::unexpected("Pointer evidence does not name the declared image RVA");
        }
    }
    if (evidence.kind == FC_EVIDENCE_DIRECT_CALL_TO || evidence.kind == FC_EVIDENCE_DIRECT_JUMP_TO) {
        const auto kind = evidence.kind == FC_EVIDENCE_DIRECT_CALL_TO ? FC_REDIRECT_CALL : FC_REDIRECT_JUMP;
        auto target = decode_direct_branch(*observed, image.info().base + location.rva + 5, architecture, kind);
        if (!target || evidence.target_rva >= image.info().size || *target != image.info().base + evidence.target_rva) {
            return std::unexpected("Direct branch evidence does not name the declared image RVA");
        }
    }
    return {};
}

std::expected<std::uintptr_t, std::string> validate_direct_branch(const targets::ImageView& image,
                                                                  FC_Architecture architecture,
                                                                  const LocationRecord& location,
                                                                  FC_RedirectKind kind) {
    auto instruction = read_image(image, location.rva, 5, "Direct branch instruction");
    if (!instruction) {
        return std::unexpected(instruction.error());
    }
    return decode_direct_branch(*instruction, image.info().base + location.rva + 5, architecture, kind);
}

} // namespace fc::planning
