#include "FusionCutter/patching.hpp"
#include "patching.hpp"

#include <Windows.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <string>
#include <utility>

namespace {

using namespace fusioncutter;

constexpr std::size_t kPageSize = 4096;

class ExecutablePage {
  public:
    ExecutablePage() {
        data_ = static_cast<std::byte*>(VirtualAlloc(nullptr, kPageSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
        REQUIRE(data_ != nullptr);
    }

    ExecutablePage(const ExecutablePage&) = delete;
    ExecutablePage& operator=(const ExecutablePage&) = delete;

    ~ExecutablePage() {
        if (data_ != nullptr) {
            VirtualFree(data_, 0, MEM_RELEASE);
        }
    }

    void write(std::size_t offset, std::span<const std::byte> bytes) {
        REQUIRE(offset <= kPageSize);
        REQUIRE(bytes.size() <= kPageSize - offset);

        DWORD previous{};
        REQUIRE(VirtualProtect(data_ + offset, bytes.size(), PAGE_EXECUTE_READWRITE, &previous));
        std::memcpy(data_ + offset, bytes.data(), bytes.size());
        REQUIRE(FlushInstructionCache(GetCurrentProcess(), data_ + offset, bytes.size()));
        DWORD ignored{};
        REQUIRE(VirtualProtect(data_ + offset, bytes.size(), previous, &ignored));
    }

    void make_executable() {
        DWORD ignored{};
        REQUIRE(VirtualProtect(data_, kPageSize, PAGE_EXECUTE_READ, &ignored));
    }

    template <typename Function> [[nodiscard]] Function function(std::size_t offset) const noexcept {
        return reinterpret_cast<Function>(data_ + offset);
    }

    [[nodiscard]] std::byte* at(std::size_t offset) const noexcept {
        return data_ + offset;
    }

    [[nodiscard]] DWORD protection(std::size_t offset) const {
        MEMORY_BASIC_INFORMATION region{};
        REQUIRE(VirtualQuery(data_ + offset, &region, sizeof(region)) == sizeof(region));
        return region.Protect;
    }

    [[nodiscard]] ImageContext image() const noexcept {
        return {TargetImage::Game, sizeof(void*) == 4 ? Architecture::X86 : Architecture::X64,
                reinterpret_cast<std::uintptr_t>(data_), kPageSize};
    }

  private:
    std::byte* data_{};
};

class ReadOnlyPage {
  public:
    ReadOnlyPage() {
        mapping_ =
            CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READONLY, 0, static_cast<DWORD>(kPageSize), nullptr);
        REQUIRE(mapping_ != nullptr);
        data_ = static_cast<const std::byte*>(MapViewOfFile(mapping_, FILE_MAP_READ, 0, 0, kPageSize));
        REQUIRE(data_ != nullptr);
    }

    ReadOnlyPage(const ReadOnlyPage&) = delete;
    ReadOnlyPage& operator=(const ReadOnlyPage&) = delete;

    ~ReadOnlyPage() {
        if (data_ != nullptr) {
            UnmapViewOfFile(data_);
        }
        if (mapping_ != nullptr) {
            CloseHandle(mapping_);
        }
    }

    [[nodiscard]] const std::byte* data() const noexcept {
        return data_;
    }

  private:
    HANDLE mapping_{};
    const std::byte* data_{};
};

template <std::size_t Size> [[nodiscard]] BytePattern exact(const std::array<std::byte, Size>& bytes) noexcept {
    return BytePattern::exact(bytes);
}

using TestFunction = int (*)();
OriginalFunction<TestFunction> original_function;

int inline_destination() {
    return original_function.get()() + 10;
}

int redirect_destination() {
    return 9;
}

void mid_callback(MidHookContext& context) noexcept {
#if defined(_M_X64)
    context.rax = 7;
#else
    context.eax = 7;
#endif
}

[[nodiscard]] PreparedPatchPlan prepare(PatchPlan&& plan) {
    auto prepared = PreparedPatchPlan::prepare(std::move(plan));
    if (!prepared) {
        INFO(prepared.error().message);
    }
    REQUIRE(prepared);
    return std::move(*prepared);
}

void reserve_and_commit(PreparedPatchPlan& plan) {
    MutationReservations reservations;
    auto reserved = reservations.reserve(plan);
    if (!reserved) {
        INFO(reserved.error().message);
    }
    REQUIRE(reserved);

    auto committed = plan.commit();
    if (!committed) {
        INFO(committed.error().reason.message);
    }
    REQUIRE(committed);
}

} // namespace

TEST_CASE("patch plans execute each fundamental operation") {
    ExecutablePage page;
    page.make_executable();

    SECTION("checked write") {
        constexpr std::size_t kOffset = 0x100;
        const std::array original{std::byte{0xB8}, std::byte{0x01}, std::byte{},
                                  std::byte{},     std::byte{},     std::byte{0xC3}};
        const std::array replacement{std::byte{0xB8}, std::byte{0x02}, std::byte{},
                                     std::byte{},     std::byte{},     std::byte{0xC3}};
        page.write(kOffset, original);

        PatchPlan plan{"CheckedWrite", page.image()};
        plan.checked_write("change return value", static_cast<std::uint32_t>(kOffset), exact(original), replacement);
        auto prepared = prepare(std::move(plan));
        reserve_and_commit(prepared);

        REQUIRE(page.function<TestFunction>(kOffset)() == 2);
        REQUIRE(page.protection(kOffset) == PAGE_EXECUTE_READ);
        REQUIRE(prepared.rollback());
        REQUIRE(page.function<TestFunction>(kOffset)() == 1);
        REQUIRE(page.protection(kOffset) == PAGE_EXECUTE_READ);
    }

    SECTION("inline hook with typed original") {
        constexpr std::size_t kOffset = 0x180;
        const std::array function_bytes{std::byte{0xB8}, std::byte{0x05}, std::byte{},
                                        std::byte{},     std::byte{},     std::byte{0xC3}};
        const std::array hook_preimage{std::byte{0xB8}, std::byte{0x05}, std::byte{}, std::byte{}, std::byte{}};
        page.write(kOffset, function_bytes);

        PatchPlan plan{"InlineHook", page.image()};
        original_function = plan.inline_hook_with_original("detour test function", static_cast<std::uint32_t>(kOffset),
                                                           exact(hook_preimage), &inline_destination);
        auto prepared = prepare(std::move(plan));
        reserve_and_commit(prepared);

        REQUIRE(original_function);
        REQUIRE(page.function<TestFunction>(kOffset)() == 15);
        REQUIRE(prepared.rollback());
        REQUIRE_FALSE(original_function);
        REQUIRE(page.function<TestFunction>(kOffset)() == 5);
    }

    SECTION("inline hook without original") {
        constexpr std::size_t kOffset = 0x1C0;
        const std::array function_bytes{std::byte{0xB8}, std::byte{0x05}, std::byte{},
                                        std::byte{},     std::byte{},     std::byte{0xC3}};
        const std::array hook_preimage{std::byte{0xB8}, std::byte{0x05}, std::byte{}, std::byte{}, std::byte{}};
        page.write(kOffset, function_bytes);

        PatchPlan plan{"InlineHookWithoutOriginal", page.image()};
        plan.inline_hook("replace test function", static_cast<std::uint32_t>(kOffset), exact(hook_preimage),
                         &redirect_destination);
        auto prepared = prepare(std::move(plan));
        reserve_and_commit(prepared);

        REQUIRE(page.function<TestFunction>(kOffset)() == 9);
        REQUIRE(prepared.rollback());
        REQUIRE(page.function<TestFunction>(kOffset)() == 5);
    }

    SECTION("mid hook with framework context") {
        constexpr std::size_t kOffset = 0x200;
        constexpr std::size_t kHookOffset = kOffset + 5;
        const std::array function_bytes{std::byte{0xB8}, std::byte{0x01}, std::byte{},     std::byte{},
                                        std::byte{},     std::byte{0x90}, std::byte{0x90}, std::byte{0x90},
                                        std::byte{0x90}, std::byte{0x90}, std::byte{0xC3}};
        const std::array hook_preimage{std::byte{0x90}, std::byte{0x90}, std::byte{0x90}, std::byte{0x90},
                                       std::byte{0x90}};
        page.write(kOffset, function_bytes);

        PatchPlan plan{"MidHook", page.image()};
        plan.mid_hook("change accumulator", static_cast<std::uint32_t>(kHookOffset), exact(hook_preimage),
                      &mid_callback);
        auto prepared = prepare(std::move(plan));
        reserve_and_commit(prepared);

        REQUIRE(page.function<TestFunction>(kOffset)() == 7);
        REQUIRE(prepared.rollback());
        REQUIRE(page.function<TestFunction>(kOffset)() == 1);
    }

    SECTION("redirect with typed original and core-owned encoding") {
        constexpr std::size_t kOffset = 0x280;
        constexpr std::size_t kOriginalDestination = 0x300;
        const std::array destination_bytes{std::byte{0xB8}, std::byte{0x03}, std::byte{},
                                           std::byte{},     std::byte{},     std::byte{0xC3}};
        std::array caller_bytes{std::byte{0xE8}, std::byte{}, std::byte{}, std::byte{}, std::byte{}, std::byte{0xC3}};
        const auto original_displacement = static_cast<std::int32_t>(kOriginalDestination - (kOffset + 5));
        std::memcpy(caller_bytes.data() + 1, &original_displacement, sizeof(original_displacement));
        page.write(kOffset, caller_bytes);
        page.write(kOriginalDestination, destination_bytes);

#if defined(_M_X64)
        const auto redirect_distance =
            static_cast<std::int64_t>(reinterpret_cast<std::uintptr_t>(&redirect_destination)) -
            static_cast<std::int64_t>(reinterpret_cast<std::uintptr_t>(page.at(kOffset + 5)));
        REQUIRE((redirect_distance < std::numeric_limits<std::int32_t>::min() ||
                 redirect_distance > std::numeric_limits<std::int32_t>::max()));
#endif

        PatchPlan plan{"Redirect", page.image()};
        auto original = plan.redirect_call_with_original(
            "replace call destination", static_cast<std::uint32_t>(kOffset),
            BytePattern::exact(std::span{caller_bytes}.first<5>()), &redirect_destination);
        auto prepared = prepare(std::move(plan));
        REQUIRE_FALSE(original);
        reserve_and_commit(prepared);

        REQUIRE(original);
        REQUIRE(original.get()() == 3);
        REQUIRE(page.function<TestFunction>(kOffset)() == 9);
        REQUIRE(prepared.rollback());
        REQUIRE_FALSE(original);
        REQUIRE(page.function<TestFunction>(kOffset)() == 3);
    }

    SECTION("jump redirect with typed original") {
        constexpr std::size_t kOffset = 0x320;
        constexpr std::size_t kOriginalDestination = 0x340;
        const std::array destination_bytes{std::byte{0xB8}, std::byte{0x03}, std::byte{},
                                           std::byte{},     std::byte{},     std::byte{0xC3}};
        std::array caller_bytes{std::byte{0xE9}, std::byte{}, std::byte{}, std::byte{}, std::byte{}};
        const auto original_displacement = static_cast<std::int32_t>(kOriginalDestination - (kOffset + 5));
        std::memcpy(caller_bytes.data() + 1, &original_displacement, sizeof(original_displacement));
        page.write(kOffset, caller_bytes);
        page.write(kOriginalDestination, destination_bytes);

        PatchPlan plan{"JumpRedirect", page.image()};
        auto original =
            plan.redirect_jump_with_original("replace jump destination", static_cast<std::uint32_t>(kOffset),
                                             BytePattern::exact(caller_bytes), &redirect_destination);
        auto prepared = prepare(std::move(plan));
        reserve_and_commit(prepared);

        REQUIRE(original);
        REQUIRE(original.get()() == 3);
        REQUIRE(page.function<TestFunction>(kOffset)() == 9);
        REQUIRE(prepared.rollback());
        REQUIRE_FALSE(original);
        REQUIRE(page.function<TestFunction>(kOffset)() == 3);
    }

    SECTION("owned data with a symbolic pointer write") {
        constexpr std::size_t kPointerOffset = 0x380;
        const std::uintptr_t null_pointer{};
        page.write(kPointerOffset, std::as_bytes(std::span{&null_pointer, 1}));

        PatchPlan plan{"AllocateData", page.image()};
        const std::uint32_t initial_value = 0x1234'5678;
        auto data = plan.allocate_data<std::uint32_t>("allocate state", 1, std::span{&initial_value, 1},
                                                      AllocationProximity{0, 0x7FFF'FFFF});
        plan.checked_write("publish state pointer", static_cast<std::uint32_t>(kPointerOffset), null_pointer,
                           data.base());
        auto prepared = prepare(std::move(plan));
        reserve_and_commit(prepared);

        REQUIRE(data.data() != nullptr);
        REQUIRE(*data.data() == initial_value);
        REQUIRE(*reinterpret_cast<std::uintptr_t*>(page.at(kPointerOffset)) ==
                reinterpret_cast<std::uintptr_t>(data.data()));
        REQUIRE(prepared.rollback());
        REQUIRE(data.data() == nullptr);
        REQUIRE(*reinterpret_cast<std::uintptr_t*>(page.at(kPointerOffset)) == 0);
    }
}

TEST_CASE("native field helpers support whole values and unaligned fields", "[core][patching]") {
    std::array<std::byte, 8> storage{};
    constexpr std::uint32_t whole_value = 0x1234'5678;
    constexpr std::uint16_t field_value = 0xABCD;

    write_native_field(storage.data(), whole_value);
    write_native_field(storage.data(), 5, field_value);

    CHECK(read_native_field<std::uint32_t>(storage.data()) == whole_value);
    CHECK(read_native_field<std::uint16_t>(storage.data(), 5) == field_value);
}

TEST_CASE("native byte helpers embed architecture-sized addresses and signed displacements", "[core][patching]") {
    std::array<std::byte, 8> image_storage{};
    const ImageContext image{TargetImage::Game, sizeof(void*) == 4 ? Architecture::X86 : Architecture::X64,
                             reinterpret_cast<std::uintptr_t>(image_storage.data()), image_storage.size()};

    std::array<std::byte, sizeof(std::uintptr_t)> address_bytes{};
    embed_image_address<0>(address_bytes, image, 4);
    CHECK(read_native_field<std::uintptr_t>(address_bytes.data()) == image.base + 4);

    auto short_branch = byte_array<0xEB, 0x00>();
    embed_relative_displacement<1, std::int8_t>(short_branch, 0x1010, 0x1000);
    CHECK(std::to_integer<unsigned int>(short_branch[1]) == 0xF0);
}

TEST_CASE("typed redirect originals require a matching direct branch") {
    ExecutablePage page;
    constexpr std::size_t kOffset = 0x100;
    const std::array not_a_call{std::byte{0x90}, std::byte{0x90}, std::byte{0x90}, std::byte{0x90}, std::byte{0x90}};
    page.write(kOffset, not_a_call);

    PatchPlan plan{"InvalidRedirectOriginal", page.image()};
    auto original = plan.redirect_call_with_original("capture invalid call", static_cast<std::uint32_t>(kOffset),
                                                     exact(not_a_call), &redirect_destination);
    auto prepared = PreparedPatchPlan::prepare(std::move(plan));

    REQUIRE_FALSE(prepared);
    CHECK(prepared.error().message.find("exact direct call or jump preimage") != std::string::npos);
    CHECK_FALSE(original);
}

TEST_CASE("patch plans reject stale assumptions before writing") {
    ExecutablePage page;
    page.make_executable();
    constexpr std::size_t kOffset = 0x100;
    const std::array original{std::byte{0x10}};
    const std::array replacement{std::byte{0x20}};
    const std::array changed{std::byte{0x30}};
    page.write(kOffset, original);

    SECTION("wrong initial preimage") {
        PatchPlan plan{"WrongPreimage", page.image()};
        plan.checked_write("stale byte", static_cast<std::uint32_t>(kOffset), exact(changed), replacement);
        auto prepared = PreparedPatchPlan::prepare(std::move(plan));
        REQUIRE_FALSE(prepared);
        REQUIRE(std::to_integer<unsigned int>(*page.at(kOffset)) == std::to_integer<unsigned int>(original.front()));
    }

    SECTION("change between validation and commit") {
        PatchPlan plan{"ChangedPreimage", page.image()};
        plan.checked_write("changed byte", static_cast<std::uint32_t>(kOffset), exact(original), replacement);
        auto prepared = prepare(std::move(plan));
        MutationReservations reservations;
        REQUIRE(reservations.reserve(prepared));

        page.write(kOffset, changed);
        auto committed = prepared.commit();
        REQUIRE_FALSE(committed);
        REQUIRE_FALSE(committed.error().rollback_failed);
        REQUIRE(std::to_integer<unsigned int>(*page.at(kOffset)) == std::to_integer<unsigned int>(changed.front()));
    }
}

TEST_CASE("a partial commit rolls back only its own completed writes") {
    ExecutablePage writable;
    writable.make_executable();
    ReadOnlyPage read_only;

    const auto writable_address = reinterpret_cast<std::uintptr_t>(writable.at(0));
    const auto read_only_address = reinterpret_cast<std::uintptr_t>(read_only.data());
    const auto image_base = std::min(writable_address, read_only_address);
    const auto image_end = std::max(writable_address, read_only_address) + kPageSize;
    REQUIRE(image_end - image_base <= std::numeric_limits<std::uint32_t>::max());

    const auto writable_rva = static_cast<std::uint32_t>(writable_address - image_base);
    const auto read_only_rva = static_cast<std::uint32_t>(read_only_address - image_base);
    const std::array original{std::byte{0x10}};
    const std::array replacement{std::byte{0x20}};
    const std::array impossible_write{std::byte{0x30}};
    const std::array zero{std::byte{}};
    writable.write(0, original);

    const ImageContext image{TargetImage::Game, sizeof(void*) == 4 ? Architecture::X86 : Architecture::X64, image_base,
                             static_cast<std::size_t>(image_end - image_base)};
    PatchPlan plan{"LocalRollback", image};
    plan.checked_write("first write", writable_rva, exact(original), replacement);
    plan.checked_write("read-only write", read_only_rva, exact(zero), impossible_write);
    auto prepared = prepare(std::move(plan));
    MutationReservations reservations;
    REQUIRE(reservations.reserve(prepared));

    auto committed = prepared.commit();
    REQUIRE_FALSE(committed);
    REQUIRE_FALSE(committed.error().rollback_failed);
    REQUIRE(std::to_integer<unsigned int>(*writable.at(0)) == std::to_integer<unsigned int>(original.front()));
}

TEST_CASE("mutation reservations enforce byte ownership without blocking safe overlaps") {
    ExecutablePage page;
    page.make_executable();
    constexpr std::size_t kOffset = 0x100;
    const std::array first{std::byte{0x10}};
    const std::array second{std::byte{0x11}};
    const std::array replacement{std::byte{0x20}};
    page.write(kOffset, first);
    page.write(kOffset + 1, second);

    SECTION("mutation conflicts with a read requirement") {
        PatchPlan owner_plan{"Owner", page.image()};
        owner_plan.checked_write("owned byte", static_cast<std::uint32_t>(kOffset), exact(first), replacement);
        auto owner = prepare(std::move(owner_plan));

        PatchPlan reader_plan{"Reader", page.image()};
        reader_plan.require_bytes("required byte", static_cast<std::uint32_t>(kOffset), exact(first));
        auto reader = prepare(std::move(reader_plan));

        MutationReservations reservations;
        REQUIRE(reservations.reserve(owner));
        auto conflict = reservations.reserve(reader);
        REQUIRE_FALSE(conflict);
        REQUIRE(conflict.error().related_patch.has_value());
        REQUIRE((*conflict.error().related_patch == "Owner"));
    }

    SECTION("one plan cannot mutate bytes it also requires") {
        PatchPlan plan{"SelfOverlap", page.image()};
        plan.require_bytes("required byte", static_cast<std::uint32_t>(kOffset), exact(first));
        plan.checked_write("owned byte", static_cast<std::uint32_t>(kOffset), exact(first), replacement);
        auto prepared = prepare(std::move(plan));

        MutationReservations reservations;
        auto conflict = reservations.reserve(prepared);
        REQUIRE_FALSE(conflict);
        REQUIRE(conflict.error().related_patch.has_value());
        REQUIRE((*conflict.error().related_patch == "SelfOverlap"));
    }

    SECTION("read overlaps and adjacent mutations are allowed") {
        PatchPlan reader_one_plan{"ReaderOne", page.image()};
        reader_one_plan.require_bytes("shared requirement", static_cast<std::uint32_t>(kOffset), exact(first));
        auto reader_one = prepare(std::move(reader_one_plan));

        PatchPlan reader_two_plan{"ReaderTwo", page.image()};
        reader_two_plan.require_bytes("shared requirement", static_cast<std::uint32_t>(kOffset), exact(first));
        auto reader_two = prepare(std::move(reader_two_plan));

        MutationReservations read_reservations;
        REQUIRE(read_reservations.reserve(reader_one));
        REQUIRE(read_reservations.reserve(reader_two));

        PatchPlan first_plan{"First", page.image()};
        first_plan.checked_write("first byte", static_cast<std::uint32_t>(kOffset), exact(first), replacement);
        auto first_patch = prepare(std::move(first_plan));

        PatchPlan second_plan{"Second", page.image()};
        second_plan.checked_write("second byte", static_cast<std::uint32_t>(kOffset + 1), exact(second), replacement);
        auto second_patch = prepare(std::move(second_plan));

        MutationReservations mutation_reservations;
        REQUIRE(mutation_reservations.reserve(first_patch));
        REQUIRE(mutation_reservations.reserve(second_patch));
    }
}

TEST_CASE("symbolic data references enforce their plan-local bounds") {
    ExecutablePage page;
    page.make_executable();
    constexpr std::size_t kOffset = 0x100;
    const std::uintptr_t null_pointer{};
    page.write(kOffset, std::as_bytes(std::span{&null_pointer, 1}));

    SECTION("another plan cannot consume the handle") {
        PatchPlan allocation_plan{"AllocationOwner", page.image()};
        auto data = allocation_plan.allocate_data<std::uint32_t>("private data", 1);

        PatchPlan foreign_plan{"ForeignReference", page.image()};
        foreign_plan.checked_write("foreign pointer", static_cast<std::uint32_t>(kOffset), null_pointer, data.base());
        auto prepared = PreparedPatchPlan::prepare(std::move(foreign_plan));
        REQUIRE_FALSE(prepared);
        REQUIRE(prepared.error().message.find("another patch plan") != std::string::npos);
    }

    SECTION("an out-of-bounds offset fails before allocation") {
        PatchPlan plan{"OutOfBoundsReference", page.image()};
        auto data = plan.allocate_data<std::uint32_t>("private data", 1);
        plan.checked_write("invalid pointer", static_cast<std::uint32_t>(kOffset), null_pointer,
                           data.byte_offset(sizeof(std::uint32_t)));

        auto prepared = PreparedPatchPlan::prepare(std::move(plan));
        REQUIRE_FALSE(prepared);
        REQUIRE(prepared.error().message.find("outside its allocation") != std::string::npos);
        REQUIRE(data.data() == nullptr);
    }
}
