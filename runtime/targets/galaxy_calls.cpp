#include "galaxy_calls.hpp"

#include "lobby.hpp"
#include "native/plan.hpp"
#include "reporting/reporting.hpp"

#include <fc/native.hpp>

#include <array>
#include <cstring>
#include <initializer_list>
#include <format>
#include <mutex>
#include <utility>

namespace fc::runtime {
namespace {

constexpr std::uint8_t kChannel = 63;
constexpr std::uint32_t kReliableOrdered = 3, kRetrieveListener = 32;
constexpr std::uint32_t kFactoryTable = 0x36f09c, kNetworkingTable = 0x3704c8, kMatchmakingTable = 0x36f680, kUserTable = 0x371924,
                        kErrorManagerTable = 0x3728a8;

using Instance = fc::NativeCall<void *() noexcept, fc::abi::X86<fc::abi::Args<>>>;
using Getter = fc::NativeCall<void *(void *) noexcept, fc::abi::X86<fc::abi::Args<fc::abi::Ecx>>>;

// GalaxyID class results use a hidden output pointer; both methods return that pointer in EAX.
using UserId =
    fc::NativeCall<std::uint64_t *(void *, std::uint64_t *) noexcept,
                   fc::abi::X86<fc::abi::Args<fc::abi::Ecx, fc::abi::Stack<0>>, fc::abi::Result<fc::abi::Eax>, fc::abi::CalleeCleanup>>;
using OwnerId = fc::NativeCall<
    std::uint64_t *(void *, std::uint64_t *, std::uint64_t) noexcept,
    fc::abi::X86<fc::abi::Args<fc::abi::Ecx, fc::abi::Stack<0>, fc::abi::Stack<4>>, fc::abi::Result<fc::abi::Eax>, fc::abi::CalleeCleanup>>;
using Available = fc::NativeCall<
    bool(void *, std::uint32_t *, std::uint8_t) noexcept,
    fc::abi::X86<fc::abi::Args<fc::abi::Ecx, fc::abi::Stack<0>, fc::abi::Stack<4>>, fc::abi::Result<fc::abi::Eax>, fc::abi::CalleeCleanup>>;
using ReadPacket = fc::NativeCall<bool(void *, void *, std::uint32_t, std::uint32_t *, std::uint64_t *, std::uint8_t) noexcept,
                                  fc::abi::X86<fc::abi::Args<fc::abi::Ecx, fc::abi::Stack<0>, fc::abi::Stack<4>, fc::abi::Stack<8>,
                                                             fc::abi::Stack<12>, fc::abi::Stack<16>>,
                                               fc::abi::Result<fc::abi::Eax>, fc::abi::CalleeCleanup>>;
using PopPacket =
    fc::NativeCall<void(void *, std::uint8_t) noexcept,
                   fc::abi::X86<fc::abi::Args<fc::abi::Ecx, fc::abi::Stack<0>>, fc::abi::Result<fc::abi::Eax>, fc::abi::CalleeCleanup>>;
using SendPacket = fc::NativeCall<bool(void *, std::uint64_t, const void *, std::uint32_t, std::uint32_t, std::uint8_t) noexcept,
                                  fc::abi::X86<fc::abi::Args<fc::abi::Ecx, fc::abi::Stack<0>, fc::abi::Stack<8>, fc::abi::Stack<12>,
                                                             fc::abi::Stack<16>, fc::abi::Stack<20>>,
                                               fc::abi::Result<fc::abi::Eax>, fc::abi::CalleeCleanup>>;
using Request =
    fc::NativeCall<void(void *, std::uint64_t) noexcept,
                   fc::abi::X86<fc::abi::Args<fc::abi::Ecx, fc::abi::Stack<0>>, fc::abi::Result<fc::abi::Eax>, fc::abi::CalleeCleanup>>;
using CopyValue =
    fc::NativeCall<void(void *, std::uint64_t, const char *, char *, std::uint32_t) noexcept,
                   fc::abi::X86<fc::abi::Args<fc::abi::Ecx, fc::abi::Stack<0>, fc::abi::Stack<8>, fc::abi::Stack<12>, fc::abi::Stack<16>>,
                                fc::abi::Result<fc::abi::Eax>, fc::abi::CalleeCleanup>>;
using SetValue = fc::NativeCall<void(void *, std::uint64_t, const char *, const char *) noexcept,
                                fc::abi::X86<fc::abi::Args<fc::abi::Ecx, fc::abi::Stack<0>, fc::abi::Stack<8>, fc::abi::Stack<12>>,
                                             fc::abi::Result<fc::abi::Eax>, fc::abi::CalleeCleanup>>;
using Register = fc::NativeCall<
    void(void *, std::uint32_t, void *) noexcept,
    fc::abi::X86<fc::abi::Args<fc::abi::Ecx, fc::abi::Stack<0>, fc::abi::Stack<4>>, fc::abi::Result<fc::abi::Eax>, fc::abi::CalleeCleanup>>;

// Matches the verified facade error interface. Both strings are borrowed until the next facade operation on this thread.
class NativeError {
  public:
    virtual ~NativeError() = default;
    virtual const char *name() const noexcept = 0;
    virtual const char *message() const noexcept = 0;
};

// Checks whether the returned object's vtable is the expected table in the selected loaded DLL.
bool matches(void *object, std::uintptr_t table) noexcept {
    std::uint32_t actual{};
    if (object) {
        std::memcpy(&actual, object, sizeof(actual));
    }
    return object && actual == table;
}

// Plans a call with exact instruction bytes and, when supplied, checks that its vtable slot points to that call.
template <class Call>
fc::NativeFunction<Call> qualify(fc::Plan &plan, std::uint32_t rva, std::initializer_list<std::uint8_t> prefix, std::uint32_t table = 0,
                                 std::uint32_t slot = 0) {
    if (table) {
        static_cast<void>(plan.read<std::uint32_t>(table + slot * 4, fc::points_to(plan.at(rva))));
    }
    return plan.function<Call>(rva, fc::exact_bytes(prefix));
}

// Attributes synchronous native failures to the shared Galaxy integration.
Diagnostic failure(std::string_view message) {
    return {"Galaxy", std::string(message)};
}

} // namespace

class GalaxyCalls::State {
  public:
    State(LobbyService &lobby, fc::reporting::ReportingSession &reporting) : listener(lobby, reporting), reporting(reporting) {}

    // Matches the native listener's destructor, success and failure vtable slots for lobby data completion.
    // Success and failure receive borrowed GalaxyID arguments.
    class Listener {
      public:
        Listener(LobbyService &lobby, fc::reporting::ReportingSession &reporting) : lobby_(lobby), reporting_(reporting) {}

        virtual ~Listener() = default;

        // Marks the lobby request successful for later delivery on the control thread.
        virtual void success(const std::uint64_t &lobby) noexcept {
            if (!lobby_.complete(lobby, true)) {
                return;
            }
            // Diagnostic allocation must not interrupt a native completion callback.
            try {
                const auto text = std::format("Galaxy lobby {}: native data request completed successfully", lobby);
                reporting_.write(nullptr, FC_LOG_DEBUG, fc::detail::text(text));
            } catch (...) {
            }
        }

        // Marks the lobby request failed for later delivery on the control thread.
        virtual void failed(const std::uint64_t &lobby, std::uint32_t reason) noexcept {
            if (!lobby_.complete(lobby, false, reason)) {
                return;
            }
            try {
                const auto text = std::format("Galaxy lobby {}: native data request failed (reason code {})", lobby, reason);
                reporting_.write(nullptr, FC_LOG_DEBUG, fc::detail::text(text));
            } catch (...) {
            }
        }

      private:
        LobbyService &lobby_;
        fc::reporting::ReportingSession &reporting_;
    } listener;

    // Keeps each facade getter's call, identity and polling history together.
    struct InterfaceGetter {
        fc::NativeFunction<Getter> function;
        std::string_view name;
        std::uintptr_t table{};
        mutable std::string error;
        std::string_view absent = "native interface is missing";
    };

    // Retrieves the current factory and verifies its vtable against the selected loaded DLL.
    std::expected<void *, Diagnostic> factory() const {
        return getter_result("GetInstance", instance.call(), facade_base + kFactoryTable, "factory is not initialized");
    }

    // Propagates lookup failures without logging; success or ordinary unavailability clears previous polling errors.
    std::expected<void *, Diagnostic> interface(const InterfaceGetter &getter) const {
        const auto current = factory();
        if (current || current.error().level == FC_LOG_DEBUG) {
            poll_error(factory_error, std::nullopt);
        }
        if (!current) {
            return std::unexpected(current.error());
        }
        auto result = getter_result(getter.name, getter.function.call(*current), getter.table, getter.absent);
        if (result || result.error().level == FC_LOG_DEBUG) {
            poll_error(getter.error, std::nullopt);
        }
        return result;
    }

    // Reports changed lookup failures while keeping ordinary initialization unavailable without an error log.
    std::expected<void *, Diagnostic> poll_interface(const InterfaceGetter &getter) const {
        const auto current = factory();
        poll_lookup(factory_error, current);
        if (!current) {
            return std::unexpected(current.error());
        }
        auto result = getter_result(getter.name, getter.function.call(*current), getter.table, getter.absent);
        poll_lookup(getter.error, result);
        return result;
    }

    // Copies the facade error immediately after an operation, before another facade operation clears borrowed text.
    // Only getters allow known initialization errors as Debug availability diagnostics.
    std::optional<Diagnostic> native_error(std::string_view operation, bool require_manager = true,
                                           bool allow_uninitialized = false) const {
        const auto manager = error_manager.call();
        if (!manager) {
            return Diagnostic{"Galaxy", std::format("{} failed; error manager call failed: {}", operation, manager.error().message),
                              manager.error().windows_error};
        }
        if (!matches(*manager, facade_base + kErrorManagerTable)) {
            if (!require_manager) {
                return std::nullopt;
            }
            return failure(std::format("{} failed; error manager is missing or its vtable does not match the selected image", operation));
        }
        const auto error = last_error.call(*manager);
        if (!error) {
            return Diagnostic{"Galaxy", std::format("{} failed; GetLastError call failed: {}", operation, error.error().message),
                              error.error().windows_error};
        }
        if (!*error) {
            return std::nullopt;
        }
        const auto *detail = static_cast<const NativeError *>(*error);
        const auto *name = detail->name(), *message = detail->message();
        auto diagnostic = failure(std::format("{} failed: {}{}{}", operation, name && *name ? name : "native error",
                                              message && *message ? ": " : "", message ? message : ""));
        if (allow_uninitialized && name && message && std::string_view{name} == "GalaxyInvalidStateError" &&
            (std::string_view{message} == "Galaxy not initialized" || std::string_view{message} == "Not initialized")) {
            diagnostic.level = FC_LOG_DEBUG;
        }
        return diagnostic;
    }

    // Retains the NativeFunction failure, or checks the native error while its borrowed strings are still valid.
    template <class T> std::optional<Diagnostic> operation_error(std::string_view operation, const fc::Result<T> &result) const {
        if (!result) {
            return Diagnostic{"Galaxy", std::format("{} call failed: {}", operation, result.error().message), result.error().windows_error};
        }
        return native_error(operation);
    }

    // Copies failures before any later facade call; successful getters allocate no diagnostic text.
    std::expected<void *, Diagnostic> getter_result(std::string_view operation, const fc::Result<void *> &object, std::uintptr_t table,
                                                    std::string_view absent) const {
        if (object && matches(*object, table)) {
            return *object;
        }
        auto error = !object ? operation_error(operation, object) : native_error(operation, false, true);
        if (error) {
            return std::unexpected(std::move(*error));
        }
        return std::unexpected(Diagnostic{
            "Galaxy", std::format("{}: {}", operation, object && *object ? "interface vtable does not match the selected image" : absent),
            0, FC_LOG_DEBUG});
    }

    // Clears polling history on success or ordinary unavailability, allowing a later failure to be reported again.
    void poll_lookup(std::string &previous, const std::expected<void *, Diagnostic> &result) const {
        poll_error(previous, !result && result.error().level != FC_LOG_DEBUG ? std::optional{result.error()} : std::nullopt);
    }

    // Suppresses unchanged polling failures; a successful operation allows a later recurrence to be reported.
    void poll_error(std::string &previous, const std::optional<Diagnostic> &error) const {
        std::lock_guard lock(diagnostics_mutex);
        if (!error) {
            previous.clear();
            return;
        }
        const auto text = diagnostic_text(*error);
        if (previous != text) {
            previous = text;
            reporting.write(nullptr, FC_LOG_ERROR, fc::detail::text(text));
        }
    }

    std::uintptr_t facade_base{}, peer_base{};
    void *registered{};
    // A failed refresh stays unavailable even if a later getter returns the previously registered instance.
    bool listener_ready{};
    fc::reporting::ReportingSession &reporting;
    mutable std::mutex diagnostics_mutex;
    mutable std::string receive_error, user_error, owner_error, listener_error;
    mutable std::string factory_error;
    fc::NativeFunction<Instance> instance, error_manager;
    InterfaceGetter user, matchmaking, networking, server_networking, registrar;
    fc::NativeFunction<Getter> last_error;
    fc::NativeFunction<UserId> user_id;
    fc::NativeFunction<OwnerId> owner_id;
    fc::NativeFunction<Available> packet_available;
    fc::NativeFunction<ReadPacket> read_packet;
    fc::NativeFunction<PopPacket> pop_packet;
    fc::NativeFunction<SendPacket> send_packet;
    fc::NativeFunction<Request> request;
    fc::NativeFunction<CopyValue> copy_value;
    fc::NativeFunction<SetValue> set_value;
    fc::NativeFunction<Register> register_listener, unregister_listener;
};

GalaxyCalls::GalaxyCalls(LobbyService &lobby, fc::reporting::ReportingSession &reporting)
    : state_(std::make_unique<State>(lobby, reporting)) {}

GalaxyCalls::~GalaxyCalls() {
    // Installed handlers remain alive until process exit. Unregister while the factory still returns the same native instance.
    if (state_->registered) {
        const auto registrar = state_->poll_interface(state_->registrar);
        if (registrar && *registrar == state_->registered) {
            static_cast<void>(state_->unregister_listener.call(state_->registered, kRetrieveListener, &state_->listener));
        }
    }
}

void GalaxyCalls::plan(fc::Plan &input, std::span<const PlanImage> images) {
    auto &state = *state_;
    bool older{};
    for (const auto &image : images) {
        if (fc::detail::equal_id(image.name, "galaxy")) {
            state.facade_base = image.base;
        } else if (fc::detail::equal_id(image.name, "peer")) {
            state.peer_base = image.base;
            older = fc::detail::equal_id(image.profile, "GalaxyPeer.59E6304A");
        }
    }

    // Facade functions start with complete thunks that use relative jumps; vtable targets use the loaded base.
    auto facade = input.for_image("galaxy");
    state.instance = qualify<Instance>(facade, 0x2fbd, {0xe9, 0x4e, 0x16, 0x0a, 0x00});
    state.error_manager = qualify<Instance>(facade, 0x4647, {0xe9, 0x24, 0xff, 0x09, 0x00});
    state.user = {qualify<Getter>(facade, 0x57b8, {0xe9, 0x53, 0xc8, 0x06, 0x00}, kFactoryTable, 5), "GetUser",
                  state.facade_base + kUserTable};
    state.matchmaking = {qualify<Getter>(facade, 0x1645, {0xe9, 0xe6, 0x05, 0x07, 0x00}, kFactoryTable, 8), "GetMatchmaking",
                         state.facade_base + kMatchmakingTable};
    state.networking = {qualify<Getter>(facade, 0x479b, {0xe9, 0xc0, 0xd5, 0x06, 0x00}, kFactoryTable, 9), "GetNetworking",
                        state.facade_base + kNetworkingTable};
    state.server_networking = {qualify<Getter>(facade, 0x459d, {0xe9, 0x7e, 0xd8, 0x06, 0x00}, kFactoryTable, 10), "GetServerNetworking",
                               state.facade_base + kNetworkingTable};
    const auto registrar = qualify<Getter>(facade, 0x4a25, {0xe9, 0xc6, 0xc7, 0x06, 0x00}, kFactoryTable, 16);

    state.user_id = qualify<UserId>(facade, 0x287e, {0xe9, 0x0d, 0x47, 0x09, 0x00}, kUserTable, 2);
    state.last_error = qualify<Getter>(facade, 0x54d4, {0xe9, 0x47, 0xf1, 0x09, 0x00}, kErrorManagerTable, 1);

    state.packet_available = qualify<Available>(facade, 0x2f9f, {0xe9, 0xec, 0x12, 0x08, 0x00}, kNetworkingTable, 3);
    state.read_packet = qualify<ReadPacket>(facade, 0x5af6, {0xe9, 0x15, 0xf5, 0x07, 0x00}, kNetworkingTable, 4);
    state.pop_packet = qualify<PopPacket>(facade, 0x2b67, {0xe9, 0x44, 0x20, 0x08, 0x00}, kNetworkingTable, 5);
    state.send_packet = qualify<SendPacket>(facade, 0x3a0d, {0xe9, 0xae, 0x1a, 0x08, 0x00}, kNetworkingTable, 1);

    state.request = qualify<Request>(facade, 0x2158, {0xe9, 0x23, 0xee, 0x07, 0x00}, kMatchmakingTable, 17);
    state.copy_value = qualify<CopyValue>(facade, 0x2fc2, {0xe9, 0xf9, 0x9b, 0x07, 0x00}, kMatchmakingTable, 19);
    state.set_value = qualify<SetValue>(facade, 0x1b5e, {0xe9, 0x7d, 0x01, 0x08, 0x00}, kMatchmakingTable, 20);
    state.owner_id = qualify<OwnerId>(facade, 0x551a, {0xe9, 0x51, 0x9b, 0x07, 0x00}, kMatchmakingTable, 30);

    // The getter returns the public registrar subobject; these thunks subtract 0x34 from the native this pointer.
    auto peer = input.for_image("peer");
    const auto registrar_table = older ? 0x854444 : 0x889984;
    state.registrar = {registrar, "GetListenerRegistrar", state.peer_base + registrar_table, {}, "native listener registrar is missing"};
    state.register_listener =
        qualify<Register>(peer, older ? 0x9b35e : 0x8e12e, {0x83, 0xe9, 0x34, 0xe9, 0x7a, 0xff, 0xff, 0xff}, registrar_table, 1);
    state.unregister_listener =
        qualify<Register>(peer, older ? 0x9c10e : 0x8ef1e, {0x83, 0xe9, 0x34, 0xe9, 0x7a, 0xff, 0xff, 0xff}, registrar_table, 2);
}

std::expected<void, Diagnostic> GalaxyCalls::lobby_status() const {
    const auto matchmaking = state_->poll_interface(state_->matchmaking);
    if (!matchmaking) {
        auto error = matchmaking.error();
        error.message = "matchmaking: " + error.message;
        return std::unexpected(std::move(error));
    }
    const auto registrar = state_->poll_interface(state_->registrar);
    if (!registrar) {
        auto error = registrar.error();
        error.message = "lobby listener: " + error.message;
        return std::unexpected(std::move(error));
    }
    if (!state_->listener_ready || *registrar != state_->registered) {
        return std::unexpected(
            Diagnostic{"Galaxy", "lobby completion listener is not registered on the current native instance", 0, FC_LOG_DEBUG});
    }
    return {};
}

std::expected<void, Diagnostic> GalaxyCalls::networking_status(bool server) const {
    const auto network = state_->poll_interface(server ? state_->server_networking : state_->networking);
    if (!network) {
        auto error = network.error();
        error.message = std::format("{} networking: {}", server ? "server" : "client", error.message);
        return std::unexpected(std::move(error));
    }
    return {};
}

std::optional<std::uint64_t> GalaxyCalls::local_user() const noexcept {
    const auto user = state_->poll_interface(state_->user);
    if (!user) {
        return std::nullopt;
    }

    std::uint64_t id{};
    const auto result = state_->user_id.call(*user, &id);
    const auto error = state_->operation_error("GetGalaxyID", result);
    state_->poll_error(state_->user_error, error);
    return result && !error && id ? std::optional{id} : std::nullopt;
}

std::optional<std::uint64_t> GalaxyCalls::lobby_owner(std::uint64_t lobby) const noexcept {
    const auto matchmaking = state_->poll_interface(state_->matchmaking);
    if (!matchmaking || !lobby) {
        return std::nullopt;
    }

    std::uint64_t owner{};
    const auto result = state_->owner_id.call(*matchmaking, &owner, lobby);
    const auto error = state_->operation_error("GetLobbyOwner", result);
    state_->poll_error(state_->owner_error, error);
    return result && !error && owner ? std::optional{owner} : std::nullopt;
}

void GalaxyCalls::refresh_listener() noexcept {
    const auto registrar = state_->poll_interface(state_->registrar);
    state_->listener_ready = false;
    if (!registrar) {
        return;
    }

    // A changed factory instance needs a new registration; the listener survives the previous instance.
    if (*registrar != state_->registered) {
        const auto result = state_->register_listener.call(*registrar, kRetrieveListener, &state_->listener);
        if (!result) {
            state_->poll_error(state_->listener_error, state_->operation_error("RegisterListener", result));
            return;
        }
        state_->registered = *registrar;
    }
    state_->poll_error(state_->listener_error, std::nullopt);
    state_->listener_ready = true;
}

CarrierResult GalaxyCalls::submit(bool server, std::uint64_t recipient, Bytes bytes) const noexcept {
    const auto network = state_->poll_interface(server ? state_->server_networking : state_->networking);
    if (!network) {
        return CarrierResult::Failed;
    }

    const auto result =
        state_->send_packet.call(*network, recipient, bytes.data(), static_cast<std::uint32_t>(bytes.size()), kReliableOrdered, kChannel);
    auto error = state_->operation_error("SendP2PPacket", result);
    if (result && !*result && !error) {
        error = failure("SendP2PPacket returned false");
    }
    if (error) {
        const auto text = diagnostic_text(*error);
        state_->reporting.write(nullptr, FC_LOG_ERROR, fc::detail::text(text));
    }
    return result && *result && !error ? CarrierResult::Accepted : CarrierResult::Failed;
}

GalaxyPacket GalaxyCalls::receive(bool server, std::span<std::byte> buffer) const noexcept {
    const auto network = state_->poll_interface(server ? state_->server_networking : state_->networking);
    if (!network) {
        return {GalaxyPacketState::Failed};
    }

    std::uint32_t size{};
    const auto available = state_->packet_available.call(*network, &size, kChannel);
    const auto error = state_->operation_error("IsP2PPacketAvailable", available);
    if (!available || error) {
        state_->poll_error(state_->receive_error, error);
        return {GalaxyPacketState::Failed};
    }
    if (!*available) {
        state_->poll_error(state_->receive_error, std::nullopt);
        return {GalaxyPacketState::Empty};
    }

    // Native Read consumes a packet even when the buffer truncates it; pop oversized packets before reading.
    if (size > buffer.size()) {
        const auto popped = state_->pop_packet.call(*network, kChannel);
        const auto pop_error = state_->operation_error("PopP2PPacket", popped);
        state_->poll_error(state_->receive_error, pop_error);
        return {popped && !pop_error ? GalaxyPacketState::Dropped : GalaxyPacketState::Failed};
    }

    std::uint64_t sender{};
    const auto read =
        state_->read_packet.call(*network, buffer.data(), static_cast<std::uint32_t>(buffer.size()), &size, &sender, kChannel);
    auto read_error = state_->operation_error("ReadP2PPacket", read);
    if (read && !*read && !read_error) {
        read_error = failure("ReadP2PPacket returned false after reporting an available packet");
    } else if (read && *read && !read_error && size > buffer.size()) {
        read_error = failure("ReadP2PPacket reported a packet larger than the supplied buffer");
    }
    state_->poll_error(state_->receive_error, read_error);
    if (!read || !*read || read_error || size > buffer.size()) {
        return {GalaxyPacketState::Failed};
    }
    return {GalaxyPacketState::Read, size, sender};
}

std::expected<void, Diagnostic> GalaxyCalls::request(std::uint64_t lobby) const {
    const auto matchmaking = state_->interface(state_->matchmaking);
    if (!matchmaking) {
        return std::unexpected(matchmaking.error());
    }
    if (!state_->listener_ready) {
        return std::unexpected(failure("Lobby data requests are unavailable"));
    }
    const auto registrar = state_->interface(state_->registrar);
    if (!registrar) {
        return std::unexpected(registrar.error());
    }
    if (*registrar != state_->registered) {
        return std::unexpected(failure("Lobby completion listener is not registered on the current native instance"));
    }

    const auto result = state_->request.call(*matchmaking, lobby);
    const auto error = state_->operation_error("RequestLobbyData", result);
    if (!result || error) {
        return std::unexpected(*error);
    }
    const auto text = std::format("Galaxy lobby {}: native data request submitted", lobby);
    state_->reporting.write(nullptr, FC_LOG_DEBUG, fc::detail::text(text));
    return {};
}

std::expected<std::string, Diagnostic> GalaxyCalls::read(std::uint64_t lobby, std::string_view key) const {
    const auto matchmaking = state_->interface(state_->matchmaking);
    if (!matchmaking) {
        return std::unexpected(matchmaking.error());
    }

    const std::string native_key(key);
    std::array<char, 4096> value{};
    const auto result =
        state_->copy_value.call(*matchmaking, lobby, native_key.c_str(), value.data(), static_cast<std::uint32_t>(value.size()));
    const auto error = state_->operation_error("GetLobbyDataCopy", result);
    if (!result || error) {
        return std::unexpected(*error);
    }
    return std::string(value.data());
}

std::expected<void, Diagnostic> GalaxyCalls::write(std::uint64_t lobby, std::string_view key, std::optional<std::string_view> value) const {
    const auto matchmaking = state_->interface(state_->matchmaking);
    if (!matchmaking) {
        return std::unexpected(matchmaking.error());
    }

    const std::string native_key(key);
    // Native Set withdraws null or empty values; LobbyService encodes published values as nonempty text.
    const std::string native_value(value.value_or(std::string_view{}));
    const auto result = state_->set_value.call(*matchmaking, lobby, native_key.c_str(), value ? native_value.c_str() : nullptr);
    const auto error = state_->operation_error("SetLobbyData", result);
    if (!result || error) {
        return std::unexpected(*error);
    }
    return {};
}

} // namespace fc::runtime
