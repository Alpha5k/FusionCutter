#include "reporting/crash_reporter.hpp"
#include "patches/patch_record.hpp"
#include "../test_support.hpp"

#include <catch2/catch_test_macros.hpp>

#include <Windows.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>

namespace {

// Attempts a volatile read; nullptr produces the fixture's read access violation.
__declspec(noinline) void FC_CDECL read_invalid(void *context) noexcept {
    const auto value = *static_cast<volatile unsigned *>(context);
    (void)value;
}

// Produces the fixture's write access violation at a null address.
__declspec(noinline) void FC_CDECL write_invalid(void *context) noexcept {
    *static_cast<volatile unsigned *>(context) = 1;
}

// Stores the faulting instruction before selecting the enclosing SEH handler.
int instruction_filter(EXCEPTION_POINTERS *exception, std::uintptr_t *instruction) noexcept {
    *instruction = reinterpret_cast<std::uintptr_t>(exception->ExceptionRecord->ExceptionAddress);
    return EXCEPTION_EXECUTE_HANDLER;
}

// Discovers the actual faulting instruction independently of the compiler's callback layout.
std::uintptr_t fault_instruction(FC_ExpectedFaultCallback callback) noexcept {
    std::uintptr_t result{};
    __try {
        callback(nullptr);
    } __except (instruction_filter(GetExceptionInformation(), &result)) {
    }
    return result;
}

// Returns whether a mismatched read fault escapes to the test's SEH handler.
bool mismatch(std::uintptr_t instruction, std::uint32_t kind) noexcept {
    __try {
        fc::runtime::CrashReporter::invoke_expected_fault(instruction, kind, &read_invalid, nullptr);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return true;
    }
    return false;
}

// Holds the inner expectation's instruction and result until the outer read recovers.
struct Nested {
    std::uintptr_t inner{};
    std::uint32_t result{};
};

// Recovers an inner write, then faults at the still-active outer read instruction.
void FC_CDECL nested(void *context) noexcept {
    auto &value = *static_cast<Nested *>(context);
    value.result = fc::runtime::CrashReporter::invoke_expected_fault(value.inner, FC_FAULT_WRITE, &write_invalid, nullptr);
    read_invalid(nullptr);
}
} // namespace

TEST_CASE("Expected native faults match instructions and access kinds through nested scopes") {
    const auto read = fault_instruction(&read_invalid), write = fault_instruction(&write_invalid);
    REQUIRE(read != 0);
    REQUIRE(write != 0);
    CHECK(fc::runtime::CrashReporter::invoke_expected_fault(read, FC_FAULT_READ, &read_invalid, nullptr) == 2);
    CHECK(mismatch(read, FC_FAULT_WRITE));
    CHECK(mismatch(read + 1, FC_FAULT_READ));

    Nested state{write};
    CHECK(fc::runtime::CrashReporter::invoke_expected_fault(read, FC_FAULT_READ, &nested, &state) == 2);
    CHECK(state.result == 2);
    EXCEPTION_RECORD record{};
    record.ExceptionCode = EXCEPTION_ACCESS_VIOLATION;
    record.ExceptionAddress = reinterpret_cast<void *>(read);
    record.NumberParameters = 2;
    CHECK_FALSE(fc::runtime::CrashReporter::expected_fault(record));
}

TEST_CASE("Unexpected native faults report copied ownership and continue exception handling") {
    const auto instruction = fault_instruction(&read_invalid);
    fc::tests::TemporaryDirectory directory;
    fc::runtime::CrashReporter reporter;
    REQUIRE(reporter.install(directory.path, {}));
    fc::runtime::PatchRecord owner;
    std::array records{
        fc::runtime::CrashAnnotation{.kind = fc::runtime::CrashAnnotationKind::Patch, .owner = &owner, .label = "Immutable owner"},
        fc::runtime::CrashAnnotation{.kind = fc::runtime::CrashAnnotationKind::NativeResource,
                                     .owner = &owner,
                                     .address = instruction,
                                     .size = 1,
                                     .detail = "Prepared fault site"}};
    reporter.publish(records);
    records[0].label = "Changed owner";
    records[1].detail = "Changed operation";

    CHECK(fc::runtime::CrashReporter::invoke_expected_fault(instruction, FC_FAULT_READ, &read_invalid, nullptr) == 2);
    CHECK_FALSE(std::filesystem::exists(directory.path / "FusionCutter.crash.log"));
    REQUIRE(mismatch(instruction, FC_FAULT_WRITE));
    const auto output = fc::tests::read_text(directory.path / "FusionCutter.crash.log");
    CHECK(output.find("ACCESS_VIOLATION") != std::string::npos);
    CHECK(output.find("Immutable owner") != std::string::npos);
    CHECK(output.find("Prepared fault site") != std::string::npos);
    CHECK(output.find("Changed owner") == std::string::npos);
    CHECK(output.find("Changed operation") == std::string::npos);
    CHECK(output.find("Stack frames") != std::string::npos);
}

TEST_CASE("Fatal reports retain the reason and copied patch identity") {
    fc::tests::TemporaryDirectory directory;
    fc::runtime::CrashReporter reporter;
    reporter.set_target("Qualified game");
    REQUIRE(reporter.install(directory.path, {.role = FC_ROLE_SERVER, .executable_basename = "Host.exe"}));
    fc::runtime::PatchRecord owner;
    fc::runtime::CrashAnnotation annotation{.kind = fc::runtime::CrashAnnotationKind::Patch, .owner = &owner, .label = "Plugin/Patch"};
    reporter.publish(std::span{&annotation, 1});
    annotation.label = "Changed identity";
    reporter.capture_fatal("Explicit failure reason", ERROR_ACCESS_DENIED, &owner);

    const auto output = fc::tests::read_text(directory.path / "FusionCutter.crash.log");
    CHECK(output.find("Explicit failure reason") != std::string::npos);
    CHECK(output.find("Plugin/Patch") != std::string::npos);
    CHECK(output.find("Changed identity") == std::string::npos);
    CHECK(output.find("Windows error: 5") != std::string::npos);
    CHECK(output.find("Qualified game") != std::string::npos);
}
