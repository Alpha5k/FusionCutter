#include "galaxy.hpp"

#include "galaxy_calls.hpp"
#include "native_helpers.hpp"
#include "core_runtime.hpp"
#include "lobby.hpp"
#include "message_protocol.hpp"
#include "native/plan.hpp"
#include "patches/patch_record.hpp"

#include <fc/detail/handler_adapter.hpp>
#include <fc/native.hpp>
#include <fc/reporting.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace fc::runtime {
namespace {

constexpr std::size_t kRecordStride = 96, kClientSlots = 64;

struct GameLayout {
    std::string_view profile;
    std::uint32_t connect, disconnect, create, receive;
    std::uint32_t carrier, carrier_getter, local_slot, local_getter;
    std::uint32_t connected, identity, owned, joined, owned_setter, joined_getter;
};

constexpr std::array kLayouts{
    GameLayout{"BF2.GOG.59EDF52B", 0x1b3c40, 0x1b3a90, 0x1b33d0, 0x1b4170, 0x1ba39c5, 0x1b3670, 0x1ba5208, 0x1b3680, 0x1acd570, 0x1acd590,
               0x1ae9204, 0x1ae91f0, 0x1d5cb0, 0x1d6db0},
    GameLayout{"BF2.Steam.59EDE353", 0x1b2ca0, 0x1b2af0, 0x1b2430, 0x1b31c0, 0x1ba2511, 0x1b26d0, 0x1ba3d58, 0x1b26e0, 0x1acc0c0, 0x1acc0e0,
               0x1ae7d50, 0x1ae7d40, 0x1d4ce0, 0x1d5de0},
};

// Selects native locations by the executable's recognized profile.
const GameLayout *select_layout(std::string_view profile) noexcept {
    const auto found = std::ranges::find_if(kLayouts, [&](const auto &layout) {
        return fc::detail::equal_id(profile, layout.profile);
    });
    return found == kLayouts.end() ? nullptr : &*found;
}

// Copies a native ID from game storage, which may be aligned to only four bytes.
std::uint64_t copy_id(const void *source) noexcept {
    std::uint64_t value{};
    std::memcpy(&value, source, sizeof(value));
    return value;
}

// Adds expected bytes for instructions that access the game's mutable fields to the plan.
void qualify(fc::Plan &plan, std::uint32_t rva, const fc::Evidence &evidence) {
    plan.check(rva, evidence.bytes.size(), evidence);
}

// Returns the profile selected for the owner's named image.
std::string_view profile(const PatchRecord &owner, std::string_view label) noexcept {
    const auto found = std::ranges::find_if(owner.selection.images, [&](const auto &image) {
        return fc::detail::equal_id(image.label, label);
    });
    return found != owner.selection.images.end() && found->profile ? found->profile->id : std::string_view{};
}

// Reports native connections and packet intake to Multiplayer, and supplies Lobby with native data access.
class NativeGalaxy {
  public:
    NativeGalaxy(PatchRecord &owner, LobbyService &lobby, const GameLayout &layout)
        : owner_(owner), lobby_(lobby), layout_(layout), calls_(lobby, owner.runtime->reporting()) {}

    // Plans access to game fields and checks instructions used to observe connections and service packets.
    void plan(fc::Plan &input) {
        calls_.plan(input, owner_.plan->images());
        auto game = input.for_image("game");
        std::uintptr_t base{};
        for (const auto &image : owner_.plan->images()) {
            if (fc::detail::equal_id(image.name, "game")) {
                base = image.base;
            }
        }

        records_memory_ = game.access<const std::byte>(layout_.connected, (kClientSlots + 1) * kRecordStride);
        local_memory_ = game.access<const std::int32_t>(layout_.local_slot);
        carrier_memory_ = game.access<const std::uint8_t>(layout_.carrier);
        owned_memory_ = game.access<const std::byte>(layout_.owned, 8);
        joined_memory_ = game.access<const std::byte>(layout_.joined, 8);

        qualify(game, layout_.carrier_getter, targets::reference(base, {0xa0}, layout_.carrier, {0xc3}));
        qualify(game, layout_.local_getter, targets::reference(base, {0x8b, 0x04, 0x8d}, layout_.local_slot, {0xc3}));
        qualify(game, layout_.connect + 8, targets::reference(base, {0x89, 0x81}, layout_.identity));
        qualify(game, layout_.connect + 36, targets::reference(base, {0xc6, 0x81}, layout_.connected, {0x01}));
        qualify(game, layout_.disconnect + 55, targets::reference(base, {0xc6, 0x86}, layout_.connected, {0x00}));
        qualify(game, layout_.create + 0x92, targets::reference(base, {0xba}, layout_.connected + 0x44, {0xbe, 0x43, 0x00, 0x00, 0x00}));
        qualify(game, layout_.create + 0xa7, fc::exact_bytes({0x84, 0xdb, 0x75, 0x3b, 0x88, 0x5a, 0xbc}));
        qualify(game, layout_.owned_setter + 21, targets::reference(base, {0xa3}, layout_.owned));
        qualify(game, layout_.owned_setter + 29, targets::reference(base, {0xa3}, layout_.owned + 4));
        qualify(game, layout_.joined_getter, targets::reference(base, {0xa1}, layout_.joined, {0x89, 0x01}));
        qualify(game, layout_.joined_getter + 7,
                targets::reference(base, {0xa1}, layout_.joined + 4, {0x89, 0x41, 0x04, 0x8b, 0xc1, 0xc3}));

        using Connect = fc::NativeCall<void(std::uint32_t, const void *) noexcept, fc::abi::X86<fc::abi::Args<fc::abi::Ecx, fc::abi::Edx>>>;
        // Connect has already written the record when this observer reads it.
        game.observe<Connect>(layout_.connect, fc::after([this](const std::uint32_t &slot, const void *const &) noexcept {
                                  observe_slot(slot);
                              }),
                              fc::exact_bytes({0x8b, 0x02, 0x8d, 0x0c, 0x49}));

        using Disconnect = fc::NativeCall<void(std::uint32_t) noexcept, fc::abi::X86<fc::abi::Args<fc::abi::Ecx>>>;
        // A disconnect ends the FC connection even if native code later reuses the same slot and Galaxy ID.
        game.observe<Disconnect>(layout_.disconnect, fc::after([this](const std::uint32_t &slot) noexcept {
                                     if (slot <= kClientSlots) {
                                         multiplayer().disconnect(slot);
                                     }
                                 }),
                                 fc::exact_bytes({0x55, 0x8b, 0xec, 0x83, 0xe4, 0xf8}));

        using Create = fc::NativeCall<void(bool) noexcept, fc::abi::X86<fc::abi::Args<fc::abi::Ecx>>>;
        // Create(false) clears native records; preserve=true keeps current connections.
        game.observe<Create>(layout_.create, fc::after([this](const bool &preserve) noexcept {
                                 if (!preserve) {
                                     multiplayer().reset_connections();
                                 }
                             }),
                             fc::exact_bytes({0x55, 0x8b, 0xec, 0x83, 0xe4, 0xf8}));

        using Receive = fc::NativeCall<void() noexcept, fc::abi::X86<fc::abi::Args<>>>;
        // Service FC packets after the game services its own receive work.
        game.observe<Receive>(layout_.receive, fc::after([this]() noexcept {
                                  service_native();
                              }),
                              fc::exact_bytes({0x55, 0x8b, 0xec, 0x51, 0x53}));
    }

    // Makes all field views usable before the installer exposes the first hook.
    fc::Result<> prepare(fc::PrepareContext &) {
        if (auto result = targets::retain_view(records_memory_, records_); !result) {
            return result;
        }
        if (auto result = targets::retain_view(local_memory_, local_slot_); !result) {
            return result;
        }
        if (auto result = targets::retain_view(carrier_memory_, carrier_); !result) {
            return result;
        }
        if (auto result = targets::retain_view(owned_memory_, owned_); !result) {
            return result;
        }
        return targets::retain_view(joined_memory_, joined_);
    }

    // Attaches native Lobby access after installation. Game Start determines the frozen feature set.
    void activate(fc::ActivateContext &) noexcept {
        lobby_.attach({this, &native_state, &request, &read, &write});
        refresh_control();
    }

    // Refreshes listener registration and native availability on the control thread.
    // The runtime then pumps Lobby and delivers callbacks.
    void update(const fc::UpdateContext &) noexcept {
        refresh_control();
    }

  private:
    MultiplayerService &multiplayer() const noexcept {
        return owner_.runtime->multiplayer();
    }

    // Copies the primary local slot and Galaxy user ID; Dedicated hosts have no local client slot.
    PeerInfo local_info(GameState game) const noexcept {
        PeerInfo info;
        if (game.mode != GameMode::Dedicated && *local_slot_ >= 0 && static_cast<std::uint32_t>(*local_slot_) < kClientSlots) {
            info.slot = static_cast<std::uint32_t>(*local_slot_);
        }
        info.galaxy_id = calls_.local_user();
        return info;
    }

    // Reports client slots 0–63 when hosting, or host route 64 when joining, to Multiplayer.
    void observe_slot(std::uint32_t slot) noexcept {
        const auto game = owner_.runtime->game().current();
        if (!game || *carrier_ || (game->is_host() ? slot >= kClientSlots : !game->is_client() || slot != kClientSlots)) {
            return;
        }

        const auto *record = records_ + slot * kRecordStride;
        if (record[0] == std::byte{}) {
            multiplayer().disconnect(slot);
            return;
        }

        const auto raw = copy_id(record + layout_.identity - layout_.connected);
        const bool local = game->mode == GameMode::Host && *local_slot_ >= 0 && slot == static_cast<std::uint32_t>(*local_slot_);
        const auto user = game->is_client() ? calls_.lobby_owner(raw)
                          : local           ? calls_.local_user()
                          : raw             ? std::optional{raw}
                                            : std::nullopt;
        multiplayer().connect(slot, raw, user, local);
    }

    // Samples connected records during native packet servicing; disconnect hooks preserve reconnects between samples.
    void seed(GameState game) noexcept {
        multiplayer().observe_local(local_info(game));
        if (game.is_host()) {
            for (std::uint32_t slot = 0; slot < kClientSlots; ++slot) {
                observe_slot(slot);
            }
        } else {
            observe_slot(kClientSlots);
        }
    }

    // Processes one packet batch; a nonblocking flag prevents concurrent or recursive reads.
    void service_native() noexcept {
        if (servicing_.test_and_set(std::memory_order_acquire)) {
            return;
        }

        struct Release {
            std::atomic_flag &flag;

            // Releases the servicing flag when the packet batch ends, including early returns.
            ~Release() {
                flag.clear(std::memory_order_release);
            }
        } release{servicing_};

        const auto game = owner_.runtime->game().current();
        if (!game || *carrier_ || (!game->is_host() && !game->is_client())) {
            return;
        }
        const bool server = game->is_host();
        if (!calls_.networking_status(server)) {
            if (native_ready_) {
                multiplayer().carrier_failed("Galaxy networking became unavailable");
            }
            return;
        }

        native_ready_ = true;
        seed(*game);

        const auto now = MultiplayerService::Clock::now();
        std::array<std::byte, message_protocol::kFrameBytes> frame{};
        for (unsigned received = 0; received < 16; ++received) {
            const auto packet = calls_.receive(server, frame);
            if (packet.state == GalaxyPacketState::Failed) {
                multiplayer().carrier_failed("Galaxy could not receive an FC packet");
                return;
            }
            if (packet.state == GalaxyPacketState::Empty) {
                break;
            }
            if (packet.state == GalaxyPacketState::Read) {
                multiplayer().receive(server, packet.sender, std::span(frame).first(packet.size), now);
            }
        }

        multiplayer().pump({this,
                            [](void *context, bool server, std::uint64_t recipient, Bytes bytes) noexcept {
                                return static_cast<NativeGalaxy *>(context)->calls_.submit(server, recipient, bytes);
                            }},
                           now);
    }

    // Reads hosted/joined lobby IDs after checking Galaxy access; verifies local ownership against the current user.
    LobbyNativeState lobby_state() const noexcept {
        const auto owned = copy_id(owned_), joined = copy_id(joined_);
        LobbyNativeState state{true, owned ? owned : joined, 0};
        if (owned) {
            const auto user = calls_.local_user(), owner = calls_.lobby_owner(owned);
            if (user && owner && *user == *owner) {
                state.owned = owned;
            }
        }
        return state;
    }

    // Checks current Galaxy access before supplying Lobby with fresh game fields and local ownership.
    static LobbyNativeState native_state(void *context) noexcept {
        auto &bridge = *static_cast<NativeGalaxy *>(context);
        return bridge.calls_.lobby_status() ? bridge.lobby_state() : LobbyNativeState{};
    }

    // Requests lobby data through the registered completion listener.
    static std::expected<void, Diagnostic> request(void *context, std::uint64_t lobby) {
        return static_cast<NativeGalaxy *>(context)->calls_.request(lobby);
    }

    // Copies cached native lobby text on the control thread.
    static std::expected<std::string, Diagnostic> read(void *context, std::uint64_t lobby, std::string_view key) {
        return static_cast<NativeGalaxy *>(context)->calls_.read(lobby, key);
    }

    // Checks that the requested lobby is still locally owned before publishing or withdrawing data.
    static std::expected<void, Diagnostic> write(void *context, std::uint64_t lobby, std::string_view key,
                                                 std::optional<std::string_view> value) {
        auto &bridge = *static_cast<NativeGalaxy *>(context);
        const auto user = bridge.calls_.local_user(), owner = bridge.calls_.lobby_owner(lobby);
        if (!user || !owner || *user != *owner || copy_id(bridge.owned_) != lobby) {
            return std::unexpected(Diagnostic{"Galaxy", "Publishing or withdrawing lobby data requires the current locally owned lobby"});
        }

        return bridge.calls_.write(lobby, key, value);
    }

    // Invalidates pending reads when access or an existing lobby association is lost or replaced.
    // A new join preserves requests made before joining.
    void refresh_control() noexcept {
        calls_.refresh_listener();
        const auto access = calls_.lobby_status();
        const auto state = access ? lobby_state() : LobbyNativeState{};
        const auto lobby_reason = access ? std::string{} : diagnostic_text(access.error());
        if (lobby_reason_ != lobby_reason) {
            lobby_reason_ = lobby_reason;
            fc::Logger{{&owner_, &owner_.runtime->host()}}.debug("Galaxy Lobby backend: {}{}{}",
                                                                 state.available ? "Available" : "Unavailable",
                                                                 lobby_reason.empty() ? "" : "; ", lobby_reason);
        }
        if ((last_state_.available && !state.available) || (last_state_.current && last_state_.current != state.current) ||
            (last_state_.owned && last_state_.owned != state.owned)) {
            lobby_.invalidate();
        }
        last_state_ = state;

        const auto game = owner_.runtime->game().current();
        std::string network_reason;
        if (!game || (!game->is_host() && !game->is_client())) {
            network_reason = "no active multiplayer session";
        } else if (*carrier_) {
            network_reason = "the game selected sockets/LAN transport";
        } else if (const auto network = calls_.networking_status(game->is_host()); !network) {
            network_reason = diagnostic_text(network.error());
        }
        const bool network = network_reason.empty();
        if (network_reason_ != network_reason) {
            network_reason_ = network_reason;
            fc::Logger{{&owner_, &owner_.runtime->host()}}.debug("Galaxy multiplayer backend: {}{}{}",
                                                                 network ? "Available" : "Unavailable", network_reason.empty() ? "" : "; ",
                                                                 network_reason);
        }
        if (network) {
            // The local Galaxy user identifies a Dedicated host even with no connected clients.
            multiplayer().observe_local(local_info(*game));
        }
    }

    PatchRecord &owner_;
    LobbyService &lobby_;
    const GameLayout &layout_;
    GalaxyCalls calls_;
    fc::Memory<const std::byte> records_memory_, owned_memory_, joined_memory_;
    fc::Memory<const std::int32_t> local_memory_;
    fc::Memory<const std::uint8_t> carrier_memory_;
    const std::byte *records_{}, *owned_{}, *joined_{};
    const std::int32_t *local_slot_{};
    const std::uint8_t *carrier_{};
    std::atomic_flag servicing_{};

    // Protected by servicing_; records whether the selected native networking interface has been available.
    bool native_ready_{};
    LobbyNativeState last_state_{};
    // The control thread logs availability only when its observed reason changes.
    std::optional<std::string> network_reason_, lobby_reason_;
};

using Holder = fc::detail::HandlerHolder<NativeGalaxy>;

// Creates the Galaxy bridge for the selected game and two recognized DLLs; the owner retains their pinned images.
std::uint32_t FC_CDECL create_bridge(void *context, void *owner, const FC_Host *host, const FC_Value *, std::uint32_t, FC_Handler *out,
                                     FC_ErrorSink error) noexcept {
    *out = {};
    try {
        auto &record = *static_cast<PatchRecord *>(owner);
        const auto *layout = select_layout(profile(record, "game"));
        const auto peer = profile(record, "peer");
        if (!layout || !fc::detail::equal_id(profile(record, "galaxy"), "Galaxy.59E63011") ||
            (!fc::detail::equal_id(peer, "GalaxyPeer.59E6304A") && !fc::detail::equal_id(peer, "GalaxyPeer.5BBE22A6"))) {
            fc::detail::write_error(error, "Galaxy integration is unavailable for the selected images");
            return 0;
        }

        auto holder =
            std::make_unique<Holder>(fc::detail::StageAccess{owner, host}, record, *static_cast<LobbyService *>(context), *layout);
        fc::detail::bind_handler(std::move(holder), out);
        return 1;
    } catch (const std::exception &exception) {
        fc::detail::write_error(error, exception.what());
    } catch (...) {
        fc::detail::write_error(error, "Creating the Galaxy bridge raised an unknown exception");
    }
    return 0;
}

} // namespace

bool supports_galaxy(std::string_view game_profile) noexcept {
    return select_layout(game_profile) != nullptr;
}

FC_HandlerFactory galaxy_factory(LobbyService &lobby) noexcept {
    return {&lobby, &create_bridge};
}

} // namespace fc::runtime
