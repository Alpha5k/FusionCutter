#pragma once

#include "native/plan.hpp"
#include <fc/native.hpp>
#include <Windows.h>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>
#include <string>

namespace fc::tests::native {

// Keeps the SDK callback captures alive beside the production plan boundary.
struct PlanView {
    // These boundary records and callback captures outlive sdk, which borrows their addresses.
    FC_Plan raw;
    FC_Host host{};
    fc::detail::CallbackCaptures captures;
    fc::Plan sdk;

    // plan must outlive this view; keep the view alive while callbacks submitted through sdk can run.
    explicit PlanView(fc::runtime::SubmittedPlan &plan)
        : raw(plan.boundary()), host{sizeof(FC_Host), fc::runtime::native_api()}, sdk(&raw, {nullptr, &host}, &captures) {}
};

// Writable executable pages hold independently supplied plan bytes and image facts.
struct Pages {
    // One 64 KiB live mapping supplies readable, writable and executable addresses to production checks.
    std::byte *base = static_cast<std::byte *>(VirtualAlloc(nullptr, 65536, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE));

    // Initializes executable test bytes; throws std::bad_alloc if the mapping could not be allocated.
    Pages() {
        if (!base) {
            throw std::bad_alloc{};
        }

        // Fill unused bytes with one-byte NOP instructions so instruction-range checks can decode them.
        std::memset(base, 0x90, 65536);
    }

    // Releases the live mapping; all borrowing plans must already have been destroyed.
    ~Pages() {
        VirtualFree(base, 0, MEM_RELEASE);
    }

    // The returned image borrows this mapping; Pages must outlive every plan built from it.
    fc::runtime::PlanImage image(std::string profile = "fixture") const {
        return {profile, profile, reinterpret_cast<std::uintptr_t>(base), 65536, {}};
    }
};

} // namespace fc::tests::native
