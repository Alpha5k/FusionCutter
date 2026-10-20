#pragma once

#include <cstdint>

// Counts fixture callbacks and retains values observed during construction and planning.
struct VerifierCounters {
    std::uint32_t ready_constructed{}, ready_planned{}, ready_value{}, ready_observed{};
    std::uint32_t prepare{}, activate{}, update{};
};

// Copies cumulative fixture observations to the output pointer; a null pointer is ignored.
using ReadVerifierCounters = void(__cdecl *)(VerifierCounters *);
