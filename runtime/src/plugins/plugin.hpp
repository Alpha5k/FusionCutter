#pragma once

#include "diagnostic.hpp"

#include <fc/abi.h>

#include <cstdint>
#include <filesystem>
#include <expected>
#include <initializer_list>
#include <span>
#include <string>
#include <vector>

namespace fc::runtime {

// Supplies one plugin declaration to admit_definitions.
struct DefinitionCandidate {
    // Source label used in diagnostics.
    std::string owner;
    // Non-owning; this record and its nested arrays stay valid during admit_definitions.
    const FC_Plugin *definition{};
};

// Refers to a plugin declaration that passed validation.
struct AdmittedPlugin {
    // Source label or absolute DLL path.
    std::string owner;
    // Non-owning; its declaration storage stays valid for every consumer of this record.
    const FC_Plugin *definition{};
};

// Accepted plugin declarations and diagnostics from one admission attempt.
struct AdmissionResult {
    std::vector<AdmittedPlugin> admitted;
    std::vector<Diagnostic> diagnostics;
};

// Validates candidates, including setting defaults, and rejects all plugins in an ASCII case-insensitive ID conflict.
// Accepted entries preserve candidate order; their declarations and nested storage must outlive every consumer.
AdmissionResult admit_definitions(std::span<const DefinitionCandidate> candidates);

inline AdmissionResult admit_definitions(std::initializer_list<DefinitionCandidate> candidates) {
    return admit_definitions(std::span<const DefinitionCandidate>{candidates.begin(), candidates.size()});
}

// Collects embedded and external declarations for one admission, rejecting conflicts across both origins.
// Accepted DLLs stay loaded until exit; rejected DLLs release this reference. Embedded storage must outlive every consumer.
AdmissionResult load_plugins(std::span<const std::filesystem::path> paths, std::span<const DefinitionCandidate> embedded = {});

// Returns regular DLL paths from one directory in deterministic filename order; an absent directory returns an empty vector.
// Other filesystem failures return a Diagnostic. Pass the resulting paths to load_plugins for admission.
std::expected<std::vector<std::filesystem::path>, Diagnostic> discover_plugins(const std::filesystem::path &directory);

} // namespace fc::runtime
