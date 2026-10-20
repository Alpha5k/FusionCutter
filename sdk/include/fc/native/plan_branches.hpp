#pragma once

#include <fc/native/plan.hpp>
#include <fc/detail/native_layout.hpp>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <type_traits>
#include <utility>

namespace fc {

inline void Plan::write_pointer(std::uint32_t rva, Address target, Evidence evidence) const {
    write_pointer(at(rva), std::move(target), std::move(evidence));
}

inline void Plan::nop(std::uint32_t rva, std::size_t size, Evidence evidence) const {
    nop(at(rva), size, std::move(evidence));
}

inline void Plan::nop(const CodeLocation &site, std::size_t size) const {
    auto location = at(site.rva, site.name);
    location.invalid |= site.invalid;
    nop(std::move(location), size, site.evidence);
}

inline void Plan::write_pointer(Location location, Address target, Evidence evidence) const {
    fixup(location, std::move(target), false, std::move(evidence));
}

inline void Plan::write(Location location, RelativeAddress target, Evidence evidence) const {
    fixup(location, std::move(target.target), true, std::move(evidence));
}

inline void Plan::nop(Location location, std::size_t size, Evidence evidence) const {
    // A NOP request sends its byte extent without replacement data; the runtime checks instruction boundaries and fills it.
    FC_Operation operation{};
    operation.kind = FC_OPERATION_WRITE;
    operation.value.write = {location.view(), evidence.view(), {}, 1, 1, 1, size};
    submit(operation);
}

template <class T> inline void Plan::operand(Location location, std::uint32_t index, Immediate<T> value, Evidence evidence) const {
    // Retain the source integer's signedness so the runtime can check it against the decoded field's encoding.
    FC_Operation operation{};
    operation.kind = FC_OPERATION_OPERAND;
    operation.value.operand = {
        location.view(), evidence.view(), index, std::is_signed_v<T>, 0, static_cast<std::uint64_t>(value.value), {}};
    submit(operation);
}

template <class T> inline void Plan::operand(const CodeLocation &site, std::uint32_t index, Immediate<T> value) const {
    auto location = at(site.rva, site.name);
    location.invalid |= site.invalid;
    operand(std::move(location), index, value, site.evidence);
}

inline void Plan::operand(Location location, std::uint32_t index, Address target, Evidence evidence) const {
    // Known addresses encode during Plan; allocation targets wait until preparation assigns their storage.
    FC_Operation operation{};
    operation.kind = FC_OPERATION_OPERAND;
    operation.value.operand = {location.view(), evidence.view(), index, 0, 1, 0, target.view()};
    submit(operation);
}

inline void Plan::operand(const CodeLocation &site, std::uint32_t index, Address target) const {
    auto location = at(site.rva, site.name);
    location.invalid |= site.invalid;
    operand(std::move(location), index, std::move(target), site.evidence);
}

template <class T> inline void Plan::operand(std::uint32_t rva, std::uint32_t index, Immediate<T> value, Evidence evidence) const {
    operand(at(rva), index, value, std::move(evidence));
}

inline void Plan::operand(std::uint32_t rva, std::uint32_t index, Address target, Evidence evidence) const {
    operand(at(rva), index, std::move(target), std::move(evidence));
}

inline void Plan::redirect_call(std::uint32_t rva, Address target, Evidence evidence) const {
    redirect_call(at(rva), std::move(target), std::move(evidence));
}

inline void Plan::redirect_jump(std::uint32_t rva, Address target, Evidence evidence) const {
    redirect_jump(at(rva), std::move(target), std::move(evidence));
}

inline void Plan::insert_call(std::uint32_t rva, std::size_t size, Address target, Evidence evidence) const {
    insert_call(at(rva), size, std::move(target), std::move(evidence));
}

inline void Plan::insert_jump(std::uint32_t rva, std::size_t size, Address target, Evidence evidence) const {
    insert_jump(at(rva), size, std::move(target), std::move(evidence));
}

inline void Plan::redirect_call(Location location, Address target, Evidence evidence) const {
    branch(std::move(location), std::move(target), FC_BRANCH_CALL, false, 5, nullptr, std::move(evidence));
}

inline void Plan::redirect_jump(Location location, Address target, Evidence evidence) const {
    branch(std::move(location), std::move(target), FC_BRANCH_JUMP, false, 5, nullptr, std::move(evidence));
}

inline void Plan::insert_call(Location location, std::size_t size, Address target, Evidence evidence) const {
    branch(std::move(location), std::move(target), FC_BRANCH_CALL, true, size, nullptr, std::move(evidence));
}

inline void Plan::insert_jump(Location location, std::size_t size, Address target, Evidence evidence) const {
    branch(std::move(location), std::move(target), FC_BRANCH_JUMP, true, size, nullptr, std::move(evidence));
}

template <class Call>
inline NativeFunction<Call> Plan::redirect_call(Location source, const FunctionLocation<Call> &target, Evidence evidence) const {
    return typed_branch<Call>(std::move(source), target, FC_BRANCH_CALL, std::move(evidence));
}

template <class Call>
inline NativeFunction<Call> Plan::redirect_jump(Location source, const FunctionLocation<Call> &target, Evidence evidence) const {
    return typed_branch<Call>(std::move(source), target, FC_BRANCH_JUMP, std::move(evidence));
}

template <class Call>
    requires(std::is_pointer_v<Call> && std::is_function_v<std::remove_pointer_t<Call>>)
inline NativeFunction<Call> Plan::redirect_call(Location source, Call target, Evidence evidence) const {
    return typed_branch<Call>(std::move(source), target, FC_BRANCH_CALL, std::move(evidence));
}

template <class Call>
    requires(std::is_pointer_v<Call> && std::is_function_v<std::remove_pointer_t<Call>>)
inline NativeFunction<Call> Plan::redirect_jump(Location source, Call target, Evidence evidence) const {
    return typed_branch<Call>(std::move(source), target, FC_BRANCH_JUMP, std::move(evidence));
}

template <class Call>
inline NativeFunction<Call> Plan::redirect_call(const CodeLocation &source, const FunctionLocation<Call> &target) const {
    auto location = at(source.rva, source.name);
    location.invalid |= source.invalid;
    return redirect_call(std::move(location), target, source.evidence);
}

template <class Call>
    requires(std::is_pointer_v<Call> && std::is_function_v<std::remove_pointer_t<Call>>)
inline NativeFunction<Call> Plan::redirect_call(const CodeLocation &source, Call target) const {
    auto location = at(source.rva, source.name);
    location.invalid |= source.invalid;
    return redirect_call(std::move(location), target, source.evidence);
}

template <class Call>
inline NativeFunction<Call> Plan::redirect_call(std::uint32_t rva, const FunctionLocation<Call> &target, Evidence evidence) const {
    return redirect_call(at(rva), target, std::move(evidence));
}

template <class Call>
    requires(std::is_pointer_v<Call> && std::is_function_v<std::remove_pointer_t<Call>>)
inline NativeFunction<Call> Plan::redirect_call(std::uint32_t rva, Call target, Evidence evidence) const {
    return redirect_call(at(rva), target, std::move(evidence));
}

template <class Call>
inline NativeFunction<Call> Plan::redirect_jump(const CodeLocation &source, const FunctionLocation<Call> &target) const {
    auto location = at(source.rva, source.name);
    location.invalid |= source.invalid;
    return redirect_jump(std::move(location), target, source.evidence);
}

template <class Call>
    requires(std::is_pointer_v<Call> && std::is_function_v<std::remove_pointer_t<Call>>)
inline NativeFunction<Call> Plan::redirect_jump(const CodeLocation &source, Call target) const {
    auto location = at(source.rva, source.name);
    location.invalid |= source.invalid;
    return redirect_jump(std::move(location), target, source.evidence);
}

template <class Call>
inline NativeFunction<Call> Plan::redirect_jump(std::uint32_t rva, const FunctionLocation<Call> &target, Evidence evidence) const {
    return redirect_jump(at(rva), target, std::move(evidence));
}

template <class Call>
    requires(std::is_pointer_v<Call> && std::is_function_v<std::remove_pointer_t<Call>>)
inline NativeFunction<Call> Plan::redirect_jump(std::uint32_t rva, Call target, Evidence evidence) const {
    return redirect_jump(at(rva), target, std::move(evidence));
}

template <class Call, class Target> inline NativeFunction<Call> Plan::redirect_call(const CallLocation<Call> &site, Target target) const {
    return typed_branch<Call>(at(site.rva, site.name), target, FC_BRANCH_CALL, site.evidence);
}

template <class Call> inline Address Plan::target_address(const FunctionLocation<Call> &target) const {
    return address(at(target.rva, target.name), target.evidence);
}

template <class Call, class Target>
    requires(std::is_pointer_v<Target> && std::is_function_v<std::remove_pointer_t<Target>>)
inline Address Plan::target_address(Target target) const {
    static_assert(std::same_as<Call, Target>, "Redirect source and replacement signatures differ");
    return address(target);
}

template <class Call, class Target>
inline NativeFunction<Call> Plan::typed_branch(Location source, Target target, std::uint32_t kind, Evidence evidence) const {
    try {
        // Submission copies this temporary signature metadata so the retained original supports the same call layout.
        auto metadata = detail::call_metadata<Call>(false);
        auto call = metadata.view();
        return {branch(std::move(source), target_address<Call>(target), kind, false, 5, &call, std::move(evidence)), api()};
    } catch (const std::exception &error) {
        // Metadata construction failures must reject the shared plan just like runtime submission failures.
        fail(error.what());
        return {};
    }
}

inline void Plan::fixup(const Location &location, Address target, bool relative, Evidence evidence) const {
    // Relative fields are always four bytes; absolute address fields match the process pointer width.
    FC_Operation operation{};
    operation.kind = FC_OPERATION_FIXUP;
    operation.value.fixup = {location.view(), evidence.view(), target.view(), relative,
                             relative ? 4u : static_cast<std::uint32_t>(sizeof(void *))};
    submit(operation);
}

inline void *Plan::branch(Location location, Address target, std::uint32_t kind, bool insert, std::size_t size, const FC_NativeCall *call,
                          Evidence evidence) const {
    // Submission copies optional call metadata and retains an original destination resource only when requested.
    FC_Operation operation{};
    operation.kind = FC_OPERATION_BRANCH;
    operation.value.branch = {location.view(), evidence.view(), target.view(), kind, insert, size, call};
    return submit(operation);
}

} // namespace fc
