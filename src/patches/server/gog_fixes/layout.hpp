#pragma once

#include <FusionCutter/patching.hpp>

namespace fusioncutter::patches::gog_fixes::layout {

inline constexpr std::uint32_t kNoRenderRva = 0x002BB37F;
inline constexpr auto kNoRenderPreimage =
    byte_array<0xFF, 0x75, 0x08, 0x8B, 0xCE, 0xE8, 0xB7, 0x00, 0x00, 0x00, 0x84, 0xC0, 0x75, 0x19>();
inline constexpr auto kNoRenderReplacement =
    byte_array<0x90, 0x90, 0x90, 0x8B, 0xCE, 0x90, 0x90, 0x90, 0x90, 0x90, 0x30, 0xC0, 0x90, 0x90>();

// Replace the dedicated-defaults password read with the populated `/password` global.
inline constexpr std::uint32_t kPasswordRva = 0x00199BC5;
inline constexpr auto kPasswordPrefixPreimage =
    byte_array<0x6A, 0x01, 0x68, 0x80, 0x00, 0x00, 0x00, 0x8D, 0x95, 0x54, 0xFF, 0xFF, 0xFF, 0xB9>();
inline constexpr auto kPasswordPrefixReplacement =
    byte_array<0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x8D, 0x05>();
inline constexpr std::uint32_t kPasswordPointerRva = kPasswordRva + kPasswordPrefixPreimage.size();
inline constexpr auto kPasswordPointerPreimage = byte_array<0xF8, 0xA3, 0x7A, 0x00>();
inline constexpr std::uint32_t kPlaintextPasswordRva = 0x01A31C40;
inline constexpr std::uint32_t kPasswordSuffixRva = kPasswordPointerRva + kPasswordPointerPreimage.size();
inline constexpr auto kPasswordSuffixPreimage =
    byte_array<0xE8, 0xA4, 0x87, 0x00, 0x00, 0x8D, 0x85, 0x54, 0xFF, 0xFF, 0xFF>();

inline constexpr std::uint32_t kPlayerMetadataRva = 0x001D7F31;
inline constexpr auto kPlayerMetadataPreimage = byte_array<0xFF, 0x74, 0x24, 0x18>();
inline constexpr auto kPlayerMetadataReplacement = byte_array<0x6A, 0x01, 0x47, 0x90>();

inline constexpr std::uint32_t kServerTypeRva = 0x001D800F;
inline constexpr auto kServerTypePreimage = byte_array<0x6A, 0x01>();
inline constexpr auto kServerTypeReplacement = byte_array<0x6A, 0x02>();

} // namespace fusioncutter::patches::gog_fixes::layout
