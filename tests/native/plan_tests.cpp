#include "plan_test_support.hpp"
#include "native/hook_registry.hpp"
#include <catch2/catch_test_macros.hpp>
#include <algorithm>
#include <array>
#include <cstring>

using fc::tests::native::Pages;
using fc::tests::native::PlanView;

TEST_CASE("Native plans copy temporary replacement bytes and evidence") {
    Pages pages;
    std::uint32_t initial = 64;
    std::memcpy(pages.base + 0x100, &initial, 4);

    fc::runtime::SubmittedPlan plan{nullptr, {pages.image()}};
    PlanView view{plan};

    auto snapshot = view.sdk.read<std::uint32_t>(0x100, fc::expect(initial));

    REQUIRE(snapshot);
    REQUIRE(*snapshot == 64);

    // Destroy and overwrite the submitted views before preparation to verify the plan retained copies.
    {
        std::uint32_t replacement = 128;
        auto evidence = fc::expect(initial);
        view.sdk.write(0x100, replacement, evidence);
        replacement = 0;
        std::ranges::fill(evidence.bytes, std::uint8_t{0});
    }

    REQUIRE(plan.prepare());
    REQUIRE(plan.install());

    std::uint32_t observed{};
    std::memcpy(&observed, pages.base + 0x100, 4);
    REQUIRE(observed == 128);
}

TEST_CASE("Native plans bind operations to named images and recheck target evidence") {
    Pages first, second;
    std::uint32_t value = 42;
    std::memcpy(second.base + 0x100, &value, 4);

    fc::runtime::SubmittedPlan plan{nullptr, {first.image("First"), second.image("Second")}};
    PlanView view{plan};

    // Bind each view by ASCII case-insensitive requirement label before referencing another image's evidence.
    auto a = view.sdk.for_image("first"), b = view.sdk.for_image("SECOND");

    SECTION("address target evidence") {
        a.write_pointer(0x100, fc::address(b.at(0x100), fc::expect(value)));
    }

    SECTION("a separate evidence check") {
        b.check(fc::CodeLocation{0x100, "target", fc::expect(value)}, sizeof(value));
        a.write_pointer(0x100, fc::address(b.at(0x100)));
    }

    SECTION("an evidence mismatch fails the plan immediately") {
        b.check(fc::CodeLocation{0x100, "target", fc::expect(value + 1)}, sizeof(value));
        REQUIRE(plan.error());
        CHECK_FALSE(plan.prepare());
        CHECK_FALSE(plan.exposed());
        return;
    }

    SECTION("an invalid site fails the plan immediately") {
        b.check(fc::CodeLocation{0x100, "target", fc::expect(value), true}, sizeof(value));
        REQUIRE(plan.error());
        CHECK_FALSE(plan.prepare());
        CHECK_FALSE(plan.exposed());
        return;
    }

    REQUIRE(plan.prepare());

    // Change the target after preparation so installation must recheck its evidence before any write.
    value = 99;
    std::memcpy(second.base + 0x100, &value, 4);

    const auto installed = plan.install();

    REQUIRE_FALSE(installed);
    CHECK_FALSE(installed.error().unsafe);
    CHECK_FALSE(plan.exposed());
    CHECK(first.base[0x100] == std::byte{0x90});

    // Multiple images require an explicit view binding before an unqualified operation can resolve its location.
    fc::runtime::SubmittedPlan unbound{nullptr, {first.image("First"), second.image("Second")}};
    PlanView invalid{unbound};
    invalid.sdk.write(0x100, 1);
    REQUIRE(unbound.error());
}

TEST_CASE("Native allocations remain aligned and continuing writes check page permissions") {
    Pages pages;
    fc::runtime::SubmittedPlan plan{nullptr, {pages.image()}};
    PlanView view{plan};

    // Continuing handles share the allocation; its one-past-the-end address can be encoded in a pointer field.
    auto data = view.sdk.allocate<std::uint32_t>(16, {64, "fixture"});
    auto memory = view.sdk.access<std::uint32_t>(data);
    auto readonly = view.sdk.access<const std::uint32_t>(data);
    view.sdk.write_pointer(0x100, data.address(data.size()));

    REQUIRE(plan.prepare());

    // Preparation exposes aligned, zero-filled initialization storage before the plan installs.
    auto values = data.values();
    REQUIRE_FALSE(values.empty());
    REQUIRE(reinterpret_cast<std::uintptr_t>(values.data()) % 64 == 0);
    REQUIRE(std::ranges::all_of(values, [](auto value) {
        return value == 0;
    }));

    REQUIRE(memory.write(77, 8));
    REQUIRE(readonly.read(8) == 77u);
    REQUIRE_FALSE(memory.read(16));

    auto borrowed = memory.view();
    REQUIRE(borrowed);

    // Changing page protection verifies that every continuing copy checks current permissions.
    DWORD previous{};
    REQUIRE(VirtualProtect(borrowed->data(), borrowed->size_bytes(), PAGE_READONLY, &previous));
    REQUIRE_FALSE(memory.write(9, 8));

    DWORD ignored{};
    REQUIRE(VirtualProtect(borrowed->data(), borrowed->size_bytes(), previous, &ignored));

    // Installation closes the mutable initialization view while the continuing handles retain their contracts.
    REQUIRE(plan.install());
    REQUIRE(data.values().empty());
    std::uintptr_t installed_end{};
    std::memcpy(&installed_end, pages.base + 0x100, sizeof(installed_end));
    CHECK(installed_end == reinterpret_cast<std::uintptr_t>(values.data() + values.size()));
}

TEST_CASE("Continuing readers share native writes while exclusive claims reject overlap") {
    Pages pages;
    fc::runtime::SubmittedPlan writer{nullptr, {pages.image()}}, other{nullptr, {pages.image()}};
    PlanView a{writer}, b{other};
    auto writable = a.sdk.access<std::uint32_t>(0x100);
    const std::array batch{&writer, &other};

    SECTION("a continuing reader observes an installed writer") {
        auto readonly = b.sdk.access<const std::uint32_t>(0x100);
        REQUIRE(fc::runtime::validate_plans(batch, {}).empty());
        REQUIRE(writer.prepare());
        REQUIRE(writer.install());
        const std::array later{&other};
        REQUIRE(fc::runtime::validate_plans(later, writer.claims()).empty());
        REQUIRE(other.prepare());
        REQUIRE(other.install());
        REQUIRE(writable.write(77));
        CHECK(readonly.read() == 77u);
    }

    SECTION("a second writer conflicts") {
        b.sdk.access<std::uint32_t>(0x100);
        CHECK(fc::runtime::validate_plans(batch, {}).size() == 1);
    }

    SECTION("a snapshot reserves its bytes against writes") {
        REQUIRE(b.sdk.read<std::uint32_t>(0x100));
        CHECK(fc::runtime::validate_plans(batch, {}).size() == 1);
    }

    SECTION("an evidence check reserves its bytes against writes") {
        b.sdk.check(fc::CodeLocation{0x100, "checked range", fc::exact_bytes({0x90, 0x90, 0x90, 0x90})}, sizeof(std::uint32_t));
        REQUIRE_FALSE(other.error());
        CHECK(fc::runtime::validate_plans(batch, {}).size() == 1);
    }
}

TEST_CASE("Address target evidence rejects overlapping edits in a batch and after its owner installs") {
    Pages pages;
    const auto evidence = fc::byte_array<0x11, 0x22, 0x33, 0x44, 0x55, 0x66>();
    std::memcpy(pages.base + 0x100, evidence.data(), evidence.size());

    fc::runtime::SubmittedPlan owner{nullptr, {pages.image()}}, mutation{nullptr, {pages.image()}};
    PlanView view{owner}, other{mutation};

    // The pointer destination is separate from the target bytes whose evidence must remain reserved.
    view.sdk.write_pointer(0x200, fc::address(view.sdk.at(0x100), fc::exact_bytes(evidence)));
    other.sdk.write(0x104, std::uint8_t{0x77});

    // Reject the competing edit in this batch, then check the same reservation after installation.
    const std::array batch{&owner, &mutation};
    REQUIRE(fc::runtime::validate_plans(batch, {}).size() == 1);
    REQUIRE(owner.prepare());
    REQUIRE(owner.install());

    const std::array later{&mutation};
    CHECK(fc::runtime::validate_plans(later, owner.claims()).size() == 1);
    CHECK(pages.base[0x104] == std::byte{0x55});
}

TEST_CASE("Hook evidence beyond displaced instructions reserves its tail without blocking compatible entries") {
    Pages pages;
    const std::uint8_t code[]{0xb8, 7, 0, 0, 0, 0xc3};
    std::memcpy(pages.base + 0x100, code, sizeof(code));

    fc::runtime::HookRegistry hooks;
    fc::runtime::SubmittedPlan owner{nullptr, {pages.image()}, &hooks}, compatible{nullptr, {pages.image()}, &hooks},
        mutation{nullptr, {pages.image()}};
    PlanView view{owner}, shared{compatible}, other{mutation};
    using Call = int (*)();

    // The five-byte MOV is displaced; its trailing RET is also protected by the six-byte evidence.
    view.sdk.observe<Call>(view.sdk.at(0x100), fc::before([]() noexcept {}), fc::exact_bytes(code));
    shared.sdk.observe<Call>(shared.sdk.at(0x100), fc::before([]() noexcept {}));
    other.sdk.write(0x105, std::uint8_t{0x90});

    const std::array accepted{&owner, &compatible};
    CHECK(fc::runtime::validate_plans(accepted, {}).empty());

    const std::array batch{&owner, &mutation};
    REQUIRE(fc::runtime::validate_plans(batch, {}).size() == 1);
    REQUIRE(owner.prepare());
    REQUIRE(owner.install());

    const std::array later{&mutation};
    CHECK(fc::runtime::validate_plans(later, owner.claims()).size() == 1);
}

TEST_CASE("Native shared hooks accept compatible entries and reject multiple modifiers") {
    Pages pages;
    const std::uint8_t code[]{0xb8, 7, 0, 0, 0, 0xc3};
    std::memcpy(pages.base + 0x100, code, sizeof(code));
    using Call = int(__cdecl *)();
    fc::runtime::SubmittedPlan first{nullptr, {pages.image()}}, second{nullptr, {pages.image()}};
    PlanView a{first}, b{second};
    const std::array batch{&first, &second};

    SECTION("a callable shares a compatible function observer") {
        a.sdk.function<Call>(0x100, fc::exact_bytes(code));
        b.sdk.observe<Call>(0x100, fc::before([]() noexcept {}));
        CHECK(fc::runtime::validate_plans(batch, {}).empty());
    }

    SECTION("two modifiers cannot own the same entry") {
        a.sdk.hook<Call>(0x100, []() noexcept {
            return 1;
        });
        b.sdk.hook<Call>(0x100, []() noexcept {
            return 2;
        });
        const auto errors = fc::runtime::validate_plans(batch, {});
        REQUIRE(errors.size() == 1);
        CHECK(errors.front().first == &second);
    }
}
