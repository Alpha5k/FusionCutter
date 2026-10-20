#include "native.hpp"

#include "service.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <initializer_list>
#include <string_view>

namespace fc::core_plugin::lua_bindings {
namespace {

using LifecycleCall = void(FC_CDECL *)() noexcept;
using UpdateCall = bool(FC_CDECL *)() noexcept;
using StackRegistration = void(FC_CDECL *)(const char *, FcLuaFunction, int) noexcept;
using RegisterRegistration =
    fc::NativeCall<void(const char *, FcLuaFunction, int) noexcept, fc::abi::Ecx, fc::abi::Edx, fc::abi::CallerCleanup>;

// Stores the prepared memory's address in output, or forwards the error if its view is unavailable.
template <class T> fc::Result<> retain_view(const fc::Memory<T> &memory, T *&output) {
    const auto view = memory.view();
    if (!view) {
        return std::unexpected(view.error());
    }

    output = view->data();
    return {};
}

} // namespace

NativeIntegration::NativeIntegration() noexcept {
    functions_.top_offset = 0x08;
    functions_.global_offset = 0x10;
    functions_.stack_offset = 0x1c;
}

void NativeIntegration::plan(fc::Plan &plan, Service &service) {
    const auto *layout = select_layout(plan.profile());
    if (!layout) {
        plan.fail("Lua bindings are unavailable for the selected executable");
        return;
    }

    // Read the unaligned absolute operand as bytes and verify that it points to the initialization flag.
    const auto operand = plan.read_bytes(
        plan.at(layout->setup.rva + static_cast<std::uint32_t>(layout->setup.operand_offset), "Address of the Lua initialization flag"),
        sizeof(std::uint32_t), fc::points_to(plan.at(layout->initialized)));
    if (!operand) {
        return;
    }

    // Subtract the flag's RVA from its absolute address to recover the executable's image base.
    std::uint32_t flag_address{};
    std::memcpy(&flag_address, operand->data(), sizeof(flag_address));
    const auto image_base = static_cast<std::uintptr_t>(flag_address - layout->initialized);

    // Require each helper's expected instructions when requesting access to its entry point.
    for (std::size_t index = 0; index < layout->functions.size(); ++index) {
        const auto &site = layout->functions[index];
        function_memory_[index] =
            plan.access<const std::byte>(plan.at(site.rva, site.name), site.bytes.size(), instruction_evidence(site, image_base));
    }

    // Verify the instruction that stores the main state and the instruction that reads its stack base.
    for (const auto *site : {&layout->state_reference, &layout->stack_reference}) {
        plan.check(plan.at(site->rva, site->name), site->bytes.size(), instruction_evidence(*site, image_base));
    }
    state_memory_ = plan.access<lua_State *const>(plan.at(layout->state, "Current Lua state"));
    initialized_memory_ = plan.access<const std::uint8_t>(plan.at(layout->initialized, "Lua initialized flag"));

    // Apply provider declarations once native setup has created the Lua state.
    plan.observe<LifecycleCall>(plan.at(layout->setup.rva, layout->setup.name), fc::after([this, &service]() noexcept {
                                    if (auto *current = state()) {
                                        service.setup(current);
                                    }
                                }),
                                instruction_evidence(layout->setup, image_base));

    // Reapply provider declarations after the game installs a native closure with this name.
    const auto registered = [this, &service](const char *const &name, const FcLuaFunction &, const int &) noexcept {
        if (auto *current = state()) {
            service.setup(current, std::string_view{name});
        }
    };

    // Mod Tools passes all registration arguments on the stack; GOG and Steam use ECX and EDX for the first two.
    if (layout->profile == "BF2.ModTools.43EBD102") {
        plan.observe<StackRegistration>(plan.at(layout->registration.rva, layout->registration.name), fc::after(registered),
                                        instruction_evidence(layout->registration, image_base));
    } else {
        plan.observe<RegisterRegistration>(plan.at(layout->registration.rva, layout->registration.name), fc::after(registered),
                                           instruction_evidence(layout->registration, image_base));
    }

    // Invalidate retained functions and queued work before the game frees or reuses the VM's memory.
    plan.observe<LifecycleCall>(plan.at(layout->teardown.rva, layout->teardown.name), fc::before([this, &service]() noexcept {
                                    if (auto *current = state()) {
                                        service.state_ending(current);
                                    }
                                }),
                                instruction_evidence(layout->teardown, image_base));

    // Updates in both shell and mission run queued Lua work and discover a VM that predates hook installation.
    plan.observe<UpdateCall>(plan.at(layout->update.rva, layout->update.name), fc::after([this, &service](const bool &) noexcept {
                                 if (auto *current = state()) {
                                     service.pump(current);
                                 }
                             }),
                             instruction_evidence(layout->update, image_base));
}

fc::Result<> NativeIntegration::prepare() {
    std::array<const std::byte *, Layout::kFunctionCount> addresses{};
    for (std::size_t index = 0; index < function_memory_.size(); ++index) {
        if (auto result = retain_view(function_memory_[index], addresses[index]); !result) {
            return result;
        }
    }

    // Layout helpers follow FcLuaNative member order, so each verified address fills the next function pointer.
    std::size_t index{};
    functions_.checkstack = reinterpret_cast<decltype(functions_.checkstack)>(addresses[index++]);
    functions_.gettop = reinterpret_cast<decltype(functions_.gettop)>(addresses[index++]);
    functions_.settop = reinterpret_cast<decltype(functions_.settop)>(addresses[index++]);
    functions_.pushvalue = reinterpret_cast<decltype(functions_.pushvalue)>(addresses[index++]);
    functions_.replace = reinterpret_cast<decltype(functions_.replace)>(addresses[index++]);
    functions_.type = reinterpret_cast<decltype(functions_.type)>(addresses[index++]);
    functions_.toboolean = reinterpret_cast<decltype(functions_.toboolean)>(addresses[index++]);
    functions_.tonumber = reinterpret_cast<decltype(functions_.tonumber)>(addresses[index++]);
    functions_.tostring = reinterpret_cast<decltype(functions_.tostring)>(addresses[index++]);
    functions_.strlen = reinterpret_cast<decltype(functions_.strlen)>(addresses[index++]);
    functions_.touserdata = reinterpret_cast<decltype(functions_.touserdata)>(addresses[index++]);
    functions_.topointer = reinterpret_cast<decltype(functions_.topointer)>(addresses[index++]);
    functions_.pushnil = reinterpret_cast<decltype(functions_.pushnil)>(addresses[index++]);
    functions_.pushnumber = reinterpret_cast<decltype(functions_.pushnumber)>(addresses[index++]);
    functions_.pushlstring = reinterpret_cast<decltype(functions_.pushlstring)>(addresses[index++]);
    functions_.pushcclosure = reinterpret_cast<decltype(functions_.pushcclosure)>(addresses[index++]);
    functions_.pushboolean = reinterpret_cast<decltype(functions_.pushboolean)>(addresses[index++]);
    functions_.pushlightuserdata = reinterpret_cast<decltype(functions_.pushlightuserdata)>(addresses[index++]);
    functions_.newtable = reinterpret_cast<decltype(functions_.newtable)>(addresses[index++]);
    functions_.gettable = reinterpret_cast<decltype(functions_.gettable)>(addresses[index++]);
    functions_.settable = reinterpret_cast<decltype(functions_.settable)>(addresses[index++]);
    functions_.rawget = reinterpret_cast<decltype(functions_.rawget)>(addresses[index++]);
    functions_.rawset = reinterpret_cast<decltype(functions_.rawset)>(addresses[index++]);
    functions_.rawgeti = reinterpret_cast<decltype(functions_.rawgeti)>(addresses[index++]);
    functions_.rawseti = reinterpret_cast<decltype(functions_.rawseti)>(addresses[index++]);
    functions_.next = reinterpret_cast<decltype(functions_.next)>(addresses[index++]);
    functions_.getmetatable = reinterpret_cast<decltype(functions_.getmetatable)>(addresses[index++]);
    functions_.setmetatable = reinterpret_cast<decltype(functions_.setmetatable)>(addresses[index++]);
    functions_.newuserdata = reinterpret_cast<decltype(functions_.newuserdata)>(addresses[index++]);
    functions_.pcall = reinterpret_cast<decltype(functions_.pcall)>(addresses[index++]);
    functions_.loadbuffer = reinterpret_cast<decltype(functions_.loadbuffer)>(addresses[index++]);
    functions_.ref = reinterpret_cast<decltype(functions_.ref)>(addresses[index++]);
    functions_.unref = reinterpret_cast<decltype(functions_.unref)>(addresses[index++]);
    functions_.error = reinterpret_cast<decltype(functions_.error)>(addresses[index++]);
    functions_.protected_call = reinterpret_cast<decltype(functions_.protected_call)>(addresses[index++]);

    if (auto result = retain_view(state_memory_, state_); !result) {
        return result;
    }
    return retain_view(initialized_memory_, initialized_);
}

lua_State *NativeIntegration::state() const noexcept {
    return *initialized_ ? *state_ : nullptr;
}

} // namespace fc::core_plugin::lua_bindings
