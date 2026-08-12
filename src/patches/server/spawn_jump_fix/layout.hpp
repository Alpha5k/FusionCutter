#pragma once

#include <FusionCutter/patching.hpp>

namespace fusioncutter::patches::spawn_jump_fix::layout {

inline constexpr std::uint32_t kPregameEndBranchRva = 0x001C4F35;
inline constexpr std::uint8_t kStockPregameEndBranch = 0x7E;
inline constexpr std::uint8_t kEndPregameAtEquality = 0x7C;
inline constexpr std::uint32_t kVanishCallRva = 0x001C4F37;
inline constexpr auto kVanishCall = byte_array<0xE8, 0x94, 0x01, 0x00, 0x00>();

inline constexpr std::uint32_t kVanishAllPlayersRva = 0x001C50D0;
inline constexpr std::uint32_t kSpawnManagerRva = 0x01AB0FE8;
inline constexpr std::uint32_t kIsPlayingRva = 0x001C4530;
inline constexpr std::uint32_t kMaximumPlayersRva = 0x01A64408;
inline constexpr std::uint32_t kFindCharacterRva = 0x00029430;

inline constexpr std::size_t kCycleDelayOffset = 0x5C;
inline constexpr std::size_t kSlotDelayOffset = 0x7C;
inline constexpr std::size_t kCycleTimerOffset = 0x9C;
inline constexpr std::size_t kTeamWaveOffset = 0xBC;
inline constexpr std::size_t kSlotTimerOffset = 0xDC;
inline constexpr std::size_t kCharacterTeamOffset = 0x134;
inline constexpr std::size_t kCharacterRequiredWaveOffset = 0x15C;

} // namespace fusioncutter::patches::spawn_jump_fix::layout
