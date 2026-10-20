#include "plan_test_support.hpp"
#include "native/hook_registry.hpp"
#include <catch2/catch_test_macros.hpp>
#include <array>
#include <cstring>
#include <utility>
#include <vector>

using fc::tests::native::Pages;
using fc::tests::native::PlanView;

TEST_CASE("Native operand edits check signed widths and instruction boundaries") {
    Pages pages;
    // CMP ECX, imm32 gives the operand editor an independently supplied four-byte immediate field.
    const std::uint8_t compare[]{0x81, 0xf9, 0x40, 0, 0, 0};
    std::memcpy(pages.base + 0x100, compare, 6);

    fc::runtime::SubmittedPlan plan{nullptr, {pages.image()}};
    PlanView view{plan};

    view.sdk.operand(fc::CodeLocation{0x100, "compare", fc::exact_bytes(compare)}, 1, fc::immediate(std::uint32_t{128}));
    REQUIRE(plan.prepare());
    REQUIRE(plan.install());

    std::uint32_t immediate{};
    std::memcpy(&immediate, pages.base + 0x102, 4);
    REQUIRE(immediate == 128);

    // A five-byte NOP request ends inside the six-byte CMP and must fail during submission.
    fc::runtime::SubmittedPlan bad{nullptr, {pages.image()}};
    PlanView invalid{bad};
    invalid.sdk.nop(0x100, 5);
    REQUIRE(bad.error());

    // Negative scalar values retain their signed representation through Plan and preparation.
    const std::uint8_t negative_compare[]{0x83, 0xf9, 0x40};
    std::memcpy(pages.base + 0x180, negative_compare, sizeof(negative_compare));
    fc::runtime::SubmittedPlan negative{nullptr, {pages.image()}};
    PlanView signed_view{negative};
    signed_view.sdk.operand(0x180, 1, fc::immediate(-128));
    REQUIRE_FALSE(negative.error());
    REQUIRE(negative.prepare());
    REQUIRE(negative.install());
    CHECK(pages.base[0x182] == std::byte{0x80});

    fc::runtime::SubmittedPlan invalid_site{nullptr, {pages.image()}};
    PlanView invalid_view{invalid_site};
    invalid_view.sdk.operand(fc::CodeLocation{0x180, "compare", {}, true}, 1, fc::immediate(1));
    REQUIRE(invalid_site.error());

    // CMP's sign-extended imm8 cannot represent positive 128 even though the logical request uses uint32_t.
    const std::uint8_t short_compare[]{0x83, 0xf9, 0x40};
    std::memcpy(pages.base + 0x200, short_compare, 3);
    fc::runtime::SubmittedPlan overflow{nullptr, {pages.image()}};
    PlanView oversized{overflow};
    oversized.sdk.operand(oversized.sdk.at(0x200), 1, fc::immediate(std::uint32_t{128}));
    REQUIRE(overflow.error());
}

TEST_CASE("Native address operands execute relative targets and reject an out-of-range displacement") {
    Pages pages;

    // Independently callable targets make the installed displacement observable on both native architectures.
    const std::uint8_t backward[]{0xb8, 7, 0, 0, 0, 0xc3}, forward[]{0xb8, 13, 0, 0, 0, 0xc3};
    std::memcpy(pages.base + 0x100, backward, sizeof(backward));
    std::memcpy(pages.base + 0x300, forward, sizeof(forward));
    const std::uint8_t call[]{0xe8, 0, 0, 0, 0, 0xc3};
    std::memcpy(pages.base + 0x200, call, sizeof(call));
    std::memcpy(pages.base + 0x220, call, sizeof(call));

    // Encode forward and backward targets during Plan, then execute each installed CALL.
    fc::runtime::SubmittedPlan plan{nullptr, {pages.image()}};
    PlanView view{plan};
    view.sdk.operand(fc::CodeLocation{0x200, "backward call", fc::exact_bytes(call)}, 0, fc::address(view.sdk.at(0x100)));
    view.sdk.operand(0x220, 0, fc::address(view.sdk.at(0x300)));
    REQUIRE_FALSE(plan.error());
    REQUIRE(plan.prepare());
    REQUIRE(plan.install());

    CHECK(reinterpret_cast<int(FC_CDECL *)()>(pages.base + 0x200)() == 7);
    CHECK(reinterpret_cast<int(FC_CDECL *)()>(pages.base + 0x220)() == 13);

#if defined(_WIN64)
    // RIP-relative memory also uses a signed displacement before its low four bytes are written.
    const std::uint8_t load[]{0x8b, 0x05, 0, 0, 0, 0, 0xc3};
    std::memcpy(pages.base + 0x400, load, sizeof(load));
    const std::uint32_t data = 91;
    std::memcpy(pages.base + 0x380, &data, sizeof(data));

    fc::runtime::SubmittedPlan memory{nullptr, {pages.image()}};
    PlanView memory_view{memory};
    memory_view.sdk.operand(0x400, 1, fc::address(memory_view.sdk.at(0x380)));
    REQUIRE(memory.prepare());
    REQUIRE(memory.install());

    CHECK(reinterpret_cast<int(FC_CDECL *)()>(pages.base + 0x400)() == 91);
#endif

    // Short JMP reach is deterministic during Plan, including mapped images at different preferred bases.
    pages.base[0x500] = std::byte{0xeb};
    pages.base[0x501] = std::byte{0};
    for (const auto base : {std::uintptr_t{0x10000000}, std::uintptr_t{0x20000000}}) {
        std::vector<std::byte> mapped(pages.base, pages.base + 65536);
        auto image = pages.image();
        image.base = base;
        image.mapped = mapped;
        image.sections = {{"code", base, 65536, IMAGE_SCN_MEM_READ | IMAGE_SCN_MEM_EXECUTE}};
        fc::runtime::SubmittedPlan overflow{nullptr, {std::move(image)}};
        PlanView overflow_view{overflow};
        overflow_view.sdk.operand(0x500, 0, fc::address(overflow_view.sdk.at(0x700)));
        REQUIRE(overflow.error());
        CHECK_FALSE(overflow.validate());
        CHECK(mapped[0x501] == std::byte{0});
    }
}

TEST_CASE("Native instruction edits reject changed originals before installation") {
    for (const bool operand : {false, true}) {
        Pages pages;
        const std::uint8_t original[]{0x83, 0xf8, 0x01};
        std::memcpy(pages.base + 0x100, original, sizeof(original));

        fc::runtime::SubmittedPlan plan{nullptr, {pages.image()}};
        PlanView view{plan};
        if (operand) {
            view.sdk.operand(fc::CodeLocation{0x100, "compare"}, 1, fc::immediate(std::uint32_t{7}));
        } else {
            view.sdk.nop(0x100, sizeof(original));
        }

        // A longer MOV replaces the planned CMP before Prepare; the final check must retain the submitted decoding.
        pages.base[0x100] = std::byte{0xb8};
        REQUIRE(plan.prepare());

        const auto revalidated = plan.revalidate();
        REQUIRE_FALSE(revalidated);
        CHECK_FALSE(plan.install());
        CHECK_FALSE(plan.exposed());
        CHECK(pages.base[0x100] == std::byte{0xb8});
    }
}

TEST_CASE("Native redirects execute replacements and retain original call handles") {
    Pages pages;
    // Original and replacement functions return distinct constants, making redirection observable by execution.
    const std::uint8_t original[]{0xb8, 7, 0, 0, 0, 0xc3}, replacement[]{0xb8, 13, 0, 0, 0, 0xc3};
    std::memcpy(pages.base + 0x200, original, 6);
    std::memcpy(pages.base + 0x300, replacement, 6);
    pages.base[0x100] = std::byte{0xe8};
    std::int32_t relative = 0x200 - 0x105;
    std::memcpy(pages.base + 0x101, &relative, 4);
    pages.base[0x105] = std::byte{0xc3};

    using Call = int(__cdecl *)();
    fc::runtime::SubmittedPlan plan{nullptr, {pages.image()}};
    PlanView view{plan};
    const fc::CallLocation<Call> site{0x100, "dispatch", fc::direct_call_to(view.sdk.at(0x200))};
    const fc::FunctionLocation<Call> target{0x300, "replacement", fc::exact_bytes(replacement)};

    auto prior = view.sdk.redirect_call(site, target);

    // A JMP uses the same retained target evidence as the CALL.
    pages.base[0x400] = std::byte{0xe9};
    relative = 0x200 - 0x405;
    std::memcpy(pages.base + 0x401, &relative, 4);
    auto prior_jump = view.sdk.redirect_jump(fc::CodeLocation{0x400, "jump", fc::direct_jump_to(view.sdk.at(0x200))}, target);
    REQUIRE_FALSE(plan.error());
    REQUIRE(plan.prepare());

    // Prepared handles call the saved targets before installation changes the branch instructions.
    REQUIRE(prior.call() == 7);
    REQUIRE(prior_jump.call() == 7);

    REQUIRE(plan.install());
    REQUIRE(reinterpret_cast<Call>(pages.base + 0x100)() == 13);
    REQUIRE(reinterpret_cast<Call>(pages.base + 0x400)() == 13);

    // Reject replacement evidence that differs from the actual function bytes during submission.
    fc::runtime::SubmittedPlan stale{nullptr, {pages.image()}};
    PlanView invalid{stale};
    const fc::FunctionLocation<Call> bad_target{0x300, "changed replacement", fc::exact_bytes({0x90})};
    invalid.sdk.redirect_call(fc::CodeLocation{0x100, "call"}, bad_target);
    REQUIRE(stale.error());
}

TEST_CASE("Instruction evidence permits readable data tails after executable originals") {
    Pages pages;
    SYSTEM_INFO system{};
    GetSystemInfo(&system);
    const auto site = static_cast<std::uint32_t>(system.dwPageSize - 5);
    const std::uint8_t code[]{0xb8, 7, 0, 0, 0, 0xc3};
    std::memcpy(pages.base + site, code, sizeof(code));
    DWORD previous{};
    REQUIRE(VirtualProtect(pages.base + system.dwPageSize, system.dwPageSize, PAGE_READONLY, &previous));

    fc::runtime::HookRegistry hooks;
    fc::runtime::SubmittedPlan plan{nullptr, {pages.image()}, &hooks};
    PlanView view{plan};
    const auto evidence = fc::exact_bytes(code);
    SECTION("instruction writes reuse decoded originals") {
        view.sdk.nop(site, 5, evidence);
    }
    SECTION("hooks reuse displaced originals") {
        view.sdk.observe<int (*)()>(view.sdk.at(site), fc::before([]() noexcept {}), evidence);
    }

    REQUIRE_FALSE(plan.error());
    REQUIRE(plan.validate());
    CHECK(plan.revalidate());
}
