#include "game_context.hpp"

#include "native_helpers.hpp"
#include "core_runtime.hpp"
#include "game.hpp"
#include "native/plan.hpp"
#include "patches/patch_record.hpp"

#include <fc/detail/handler_adapter.hpp>
#include <fc/native.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <expected>
#include <memory>
#include <optional>
#include <string_view>
#include <utility>

namespace fc::runtime {
namespace {

constexpr std::uint32_t kMissionHash = 0xc2cbd863;

// Native session fields and event locations for one executable build.
struct MultiplayerLayout {
    // Host start, client start, then four writes that end a session.
    std::array<std::uint32_t, 6> session_sites;
    std::uint32_t in_session, native_host, host_index, dedicated;
    std::uint32_t in_session_getter, host_getter, set_state, replace_state;
    std::uint32_t carrier, carrier_getter;
    std::uint32_t accepted_close, close_object, admin_shutdown;
    std::array<std::uint8_t, 5> shutdown_call;
};

constexpr MultiplayerLayout kGogMultiplayer{{0x1dd1cb, 0x1e0b6a, 0x1dd378, 0x1e0c6f, 0x1e362f, 0x1e4571},
                                            0x1aaeddf,
                                            0x1a58ebd,
                                            0x1a643bc,
                                            0x1a6435a,
                                            0x1dca80,
                                            0x29240,
                                            0x13c7e0,
                                            0x13c800,
                                            0x1ba39c5,
                                            0x1b3670,
                                            0x218386,
                                            0x1a57844,
                                            0x1b0cc6,
                                            {0xe8, 0xc5, 0xbd, 0x02, 0x00}};
constexpr MultiplayerLayout kSteamMultiplayer{{0x1dc11b, 0x1dfaba, 0x1dc2c8, 0x1dfbbf, 0x1e257f, 0x1e34c1},
                                              0x1aad92c,
                                              0x1a57a0d,
                                              0x1a62f0c,
                                              0x1a62eaa,
                                              0x1db9d0,
                                              0x29280,
                                              0x13ba90,
                                              0x13bab0,
                                              0x1ba2511,
                                              0x1b26d0,
                                              0x2172e6,
                                              0x1a56394,
                                              0x1afd26,
                                              {0xe8, 0xb5, 0xbc, 0x02, 0x00}};

struct Layout {
    std::string_view profile;
    std::uint32_t mission_state, mission_object;
    const MultiplayerLayout *multiplayer;
};

constexpr std::array kLayouts{
    Layout{"BF2.GOG.59EDF52B", 0x1a57858, 0x3ec96c, &kGogMultiplayer},
    Layout{"BF2.Steam.59EDE353", 0x1a563a8, 0x3eb99c, &kSteamMultiplayer},
    Layout{"BF2.ModTools.43EBD102", 0x73056c, 0x6c6d88, nullptr},
};

// Returns the native layout for the recognized executable profile.
const Layout *select_layout(std::string_view profile) noexcept {
    const auto found = std::ranges::find_if(kLayouts, [&](const auto &layout) {
        return fc::detail::equal_id(profile, layout.profile);
    });
    return found == kLayouts.end() ? nullptr : &*found;
}

// Adds expected bytes for instructions that access the game's mutable fields to the plan.
void qualify(fc::Plan &plan, std::uint32_t rva, std::string_view name, const fc::Evidence &evidence) {
    plan.check(plan.at(rva, name), evidence.bytes.size(), evidence);
}

// Observes session, mission and normal exit events from qualified native hooks.
class NativeGameObserver {
  public:
    NativeGameObserver(GameContext &context, const Layout &layout, PatchRecord &owner)
        : context_(context), layout_(layout), owner_(owner) {}

    // Plans access to game fields and checks bytes at the observed instructions.
    void plan(fc::Plan &input) {
        auto plan = input.for_image("game");
        std::uintptr_t image_base{};
        for (const auto &image : owner_.plan->images()) {
            if (fc::detail::equal_id(image.name, "game")) {
                image_base = image.base;
                mission_object_ = image_base + layout_.mission_object;
            }
        }

        mission_memory_ = plan.access<const std::uint32_t>(layout_.mission_state);

        if (const auto *native = layout_.multiplayer) {
            in_session_memory_ = plan.access<const std::uint8_t>(native->in_session);
            host_memory_ = plan.access<const std::uint8_t>(native->native_host);
            index_memory_ = plan.access<const std::int32_t>(native->host_index);
            dedicated_memory_ = plan.access<const std::uint8_t>(native->dedicated);
            carrier_memory_ = plan.access<const std::uint8_t>(native->carrier);

            qualify(plan, native->carrier_getter, "Networking mode getter",
                    targets::reference(image_base, {0xa0}, native->carrier, {0xc3}));
            qualify(plan, native->in_session_getter, "Session getter",
                    targets::reference(image_base, {0x55, 0x8b, 0xec, 0xa0}, native->in_session, {0x5d, 0xc3}));
            qualify(plan, native->host_getter, "Host flag getter",
                    targets::reference(image_base, {0x80, 0x3d}, native->native_host, {0x00, 0x74, 0x03, 0xb0, 0x01, 0xc3}));
            qualify(plan, native->host_getter + 12, "Host index getter",
                    targets::reference(image_base, {0x83, 0x3d}, native->host_index, {0x00, 0x0f, 0x9d, 0xc0, 0xc3}));
            qualify(plan, native->host_getter + 32, "Dedicated mode getter",
                    targets::reference(image_base, {0xa0}, native->dedicated, {0xc3}));
            qualify(plan, native->replace_state + 3, "Mission state read",
                    targets::reference(image_base, {0x8b, 0x0d}, layout_.mission_state));
            qualify(plan, native->set_state + 12, "Mission object reference",
                    targets::reference(image_base, {0xb9}, layout_.mission_object));

            // Accepted WM_CLOSE reaches native cleanup and _exit; finalize before cleanup can terminate the process.
            plan.observe_instruction(native->accepted_close, fc::before([this](const fc::CpuContext &) noexcept {
                                         owner_.runtime->finish_reporting();
                                     }),
                                     targets::reference(image_base, {0xa1}, native->close_object));

            // Admin tools can kill the server shortly after shutdown; observe this command before its quit request.
            // The shared request callee also handles ordinary disconnects, so only this dedicated command finishes output.
            qualify(plan, native->admin_shutdown - 2, "Admin shutdown argument", fc::exact_bytes({0x6a, 0x00}));
            qualify(plan, native->admin_shutdown + 5, "Admin shutdown argument cleanup", fc::exact_bytes({0x83, 0xc4, 0x04}));
            fc::Evidence shutdown_call;
            shutdown_call.bytes.assign(native->shutdown_call.begin(), native->shutdown_call.end());
            plan.observe_instruction(native->admin_shutdown, fc::before([this](const fc::CpuContext &) noexcept {
                                         if (*dedicated_) {
                                             owner_.runtime->finish_reporting();
                                         }
                                     }),
                                     shutdown_call);

            // The writes to the session flag distinguish cancellation from successful setup cleanup.
            for (std::size_t index = 0; index < native->session_sites.size(); ++index) {
                const auto evidence =
                    targets::reference(image_base, {0xc6, 0x05}, native->in_session, {static_cast<std::uint8_t>(index < 2)});
                plan.observe_instruction(native->session_sites[index], fc::before([this, index](const fc::CpuContext &) noexcept {
                                             if (index == 0) {
                                                 context_.observe(GameEvent::Start, *dedicated_ ? GameMode::Dedicated : GameMode::Host);
                                             } else if (index == 1) {
                                                 context_.observe(GameEvent::Start, GameMode::Client);
                                             } else {
                                                 context_.observe(GameEvent::End, GameMode::None);
                                                 owner_.runtime->multiplayer().end();
                                             }

                                             if (index < 2) {
                                                 // Freeze installed features when the native session starts; Galaxy DLLs may become
                                                 // available later.
                                                 if (const auto game = context_.current()) {
                                                     owner_.runtime->multiplayer().begin(*game, !*carrier_);
                                                 }
                                             }
                                         }),
                                         evidence);
            }

            // SetState passes the hash only in ECX; the installer relocates its complete prefix, including the short branch.
            using SetState = fc::NativeCall<void(std::uint32_t) noexcept, fc::abi::X86<fc::abi::Args<fc::abi::Ecx>>>;
            plan.observe<SetState>(native->set_state, fc::before([this](const std::uint32_t &hash) noexcept {
                                       if (hash == kMissionHash || hash == 0) {
                                           context_.observe(hash == kMissionHash ? GameEvent::MissionEnter : GameEvent::MissionExit,
                                                            sample());
                                       }
                                   }),
                                   fc::exact_bytes({0x85, 0xc9, 0x74, 0x12, 0x81, 0xf9, 0x63, 0xd8, 0xcb, 0xc2}));
        } else {
            network_memory_ = plan.access<const std::uint8_t>(0x7e14f0, 2);

            qualify(plan, 0x4d780, "Scene state read", targets::reference(image_base, {0x8b, 0x0d}, layout_.mission_state));
            auto assignment = targets::reference(image_base, {0xc7, 0x05}, 0x7305ac);
            const auto mission_object = targets::reference(image_base, {}, layout_.mission_object);
            assignment.bytes.insert(assignment.bytes.end(), mission_object.bytes.begin(), mission_object.bytes.end());
            qualify(plan, 0x4d7f6, "Mission object assignment", assignment);
            qualify(plan, 0x2e184a, "Next network mode read", targets::reference(image_base, {0x8a, 0x15}, 0x7e14f1));
            qualify(plan, 0x2e1850, "Apply next network mode", targets::reference(image_base, {0x88, 0x15}, 0x7e14f0));

            // The mission pointer is set before Enter; it identifies a Local mission while the world initializes.
            plan.observe_instruction(0x4fe10, fc::before([this](const fc::CpuContext &) noexcept {
                                         context_.observe(GameEvent::MissionEnter, sample());
                                     }),
                                     fc::exact_bytes({0x83, 0xec, 0x0c, 0x53, 0x56, 0x57}));
            plan.observe_instruction(0x50090, fc::before([this](const fc::CpuContext &) noexcept {
                                         context_.observe(GameEvent::MissionExit, sample());
                                     }),
                                     targets::reference(image_base, {0xa0}, 0x94d954));
        }
    }

    // Resolves views of game fields before the installer exposes hooks.
    fc::Result<> prepare(fc::PrepareContext &) {
        if (auto result = targets::retain_view(mission_memory_, mission_); !result) {
            return result;
        }
        if (!layout_.multiplayer) {
            return targets::retain_view(network_memory_, networking_);
        }

        if (auto result = targets::retain_view(in_session_memory_, in_session_); !result) {
            return result;
        }
        if (auto result = targets::retain_view(host_memory_, host_); !result) {
            return result;
        }
        if (auto result = targets::retain_view(index_memory_, host_index_); !result) {
            return result;
        }
        if (auto result = targets::retain_view(carrier_memory_, carrier_); !result) {
            return result;
        }
        return targets::retain_view(dedicated_memory_, dedicated_);
    }

    // Samples current game state after installation. GameContext keeps any newer state already established by a hook.
    void activate(fc::ActivateContext &) noexcept {
        context_.attach(sample());
    }

  private:
    // Reads session flags and mission state; active Mod Tools networking returns unavailable.
    std::optional<GameMode> sample() const noexcept {
        // An active native session takes precedence over mission state during transitions.
        if (layout_.multiplayer) {
            if (*in_session_) {
                return (*host_ || *host_index_ >= 0) ? (*dedicated_ ? GameMode::Dedicated : GameMode::Host) : GameMode::Client;
            }
        } else if (networking_[0] || networking_[1]) {
            return std::nullopt;
        }
        return *mission_ == mission_object_ ? GameMode::Local : GameMode::None;
    }

    GameContext &context_;
    const Layout &layout_;
    PatchRecord &owner_;
    std::uintptr_t mission_object_{};
    fc::Memory<const std::uint32_t> mission_memory_;
    fc::Memory<const std::uint8_t> in_session_memory_, host_memory_, dedicated_memory_, network_memory_, carrier_memory_;
    fc::Memory<const std::int32_t> index_memory_;
    const std::uint32_t *mission_{};
    const std::uint8_t *in_session_{}, *host_{}, *dedicated_{}, *networking_{}, *carrier_{};
    const std::int32_t *host_index_{};
};

using Holder = fc::detail::HandlerHolder<NativeGameObserver>;

// Creates the Game observer and retains its callback captures through the shared handler lifecycle.
std::uint32_t FC_CDECL create_observer(void *context, void *owner, const FC_Host *host, const FC_Value *, std::uint32_t, FC_Handler *out,
                                       FC_ErrorSink error) noexcept {
    *out = {};
    try {
        auto &record = *static_cast<PatchRecord *>(owner);
        const auto image = std::ranges::find_if(record.selection.images, [](const auto &binding) {
            return fc::detail::equal_id(binding.label, "game");
        });
        const auto *layout = image != record.selection.images.end() && image->profile ? select_layout(image->profile->id) : nullptr;
        if (!layout) {
            fc::detail::write_error(error, "Game context is unavailable for the selected executable");
            return 0;
        }

        auto holder = std::make_unique<Holder>(fc::detail::StageAccess{owner, host}, *static_cast<GameContext *>(context), *layout, record);
        fc::detail::bind_handler(std::move(holder), out);
        return 1;
    } catch (const std::exception &exception) {
        fc::detail::write_error(error, exception.what());
    } catch (...) {
        fc::detail::write_error(error, "Creating the Game observer raised an unknown exception");
    }
    return 0;
}

} // namespace

bool supports_game_context(std::string_view profile) noexcept {
    return select_layout(profile) != nullptr;
}

FC_HandlerFactory game_context_factory(GameContext &context) noexcept {
    return {&context, &create_observer};
}

} // namespace fc::runtime
