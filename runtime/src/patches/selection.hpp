#pragma once

#include "images/image.hpp"
#include "plugins/plugin.hpp"

#include <cstddef>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace fc::runtime {

// Connects one declared image label to the catalog profiles accepted by its selectors.
struct ResolvedImageRequirement {
    // Refers to the admitted plugin declaration, which must outlive this record.
    const FC_ImageRequirement *definition{};
    // Unique candidates in catalog order; the catalog and its referenced data must outlive this record.
    std::vector<const ImageProfile *> profiles;
};

// Groups the resolved image requirements for one admitted patch variant.
struct ResolvedVariant {
    // Refers to the admitted plugin declaration, which must outlive this record.
    const FC_Variant *definition{};
    // Zero-based position in the patch's original variant declarations.
    std::size_t index{};
    // Requirements remain in declaration order, including those whose selectors match no profiles.
    std::vector<ResolvedImageRequirement> images;
};

// Collects the resolved variants and declaration references used to evaluate one patch repeatedly.
struct ResolvedPatch {
    // Source name supplied at plugin admission, used for resolution diagnostics.
    std::string owner;
    // The admitted plugin declaration must outlive this record.
    const FC_Plugin *plugin{};
    // Refers to one patch inside plugin; the declaration storage must remain alive and unchanged.
    const FC_Patch *definition{};
    std::vector<ResolvedVariant> variants;
};

// A nonempty optional identifies a recognized profile; an empty optional means the image is absent.
// An error preserves a failed file or loaded module inspection.
using ImageObservationOutcome = std::expected<std::optional<const ImageProfile *>, Diagnostic>;

// Records one inspected image or an absent discovery hint.
struct ImageObservation {
    // Actual basename for present images; requested discovery hint for absent or failed images.
    std::string filename;
    // A present profile must point into the same catalog instance used by resolve_patch.
    ImageObservationOutcome outcome;
    // Identifies the process executable so recognized build exclusions remain distinct from required DLL failures.
    bool executable{};
    // A nonzero live module base identifies aliases of the same mapping; zero keeps offline observations distinct.
    std::uintptr_t base{};
};

// Owns one live basename's inspection result and the ranges of any recognized, pinned module.
struct LiveImageObservation {
    // Requested basename is retained even when inspection fails or the module is absent.
    std::string filename;
    // Empty means absent; errors retain failed inspections. Recognized images keep their process-lifetime pin.
    std::expected<std::optional<LoadedImage>, Diagnostic> outcome;
    // Distinguishes recognized executable build exclusions from required DLL failures during selection.
    bool executable{};
};

using LiveImageObservations = std::vector<LiveImageObservation>;

// Reports the result of matching image requirements against observed module identities.
enum class SelectionState {
    // Every image label in the selected variant has one unambiguous binding.
    Ready,
    // A variant awaits a DLL and every other requirement is ready or waiting.
    Waiting,
    // All applicable variants are unsupported, incompatible, ambiguous, or failed; also used when none apply.
    Unavailable,
};

// Connects one local image label to the concrete profile and observation selected for it.
struct SelectedImageBinding {
    // Refers to an image label in the admitted declaration, whose storage must outlive this binding.
    std::string_view label;
    // Refers to the catalog used during resolution, which must remain alive and unchanged.
    const ImageProfile *profile{};
    // Indexes the observations input; a live projection preserves the owning live record's index.
    std::size_t observation_index{};
};

// Selected variant, image bindings, and reasons for one patch evaluation.
struct PatchSelection {
    SelectionState state{SelectionState::Unavailable};
    // Set only for Ready; refers to the matching admitted variant.
    const FC_Variant *variant{};
    // Zero-based position in the patch declaration, meaningful only for Ready.
    std::size_t variant_index{};
    // Ready bindings follow the selected variant's requirement order; other states leave this empty.
    std::vector<SelectedImageBinding> images;
    // Empty for Ready; otherwise explains the waiting or unavailable variants.
    std::vector<Diagnostic> reasons;
    // Unavailable solely because of launch role, builds outside this catalog or absent or mismatched executables.
    // This presentation fact leaves lifecycle state unchanged; inspection and declaration failures stay applicable.
    bool not_applicable{};
};

// Expands image selectors and rejects same-role overlaps established by catalog filename hints.
// Pass a valid patch_index and validated catalog; keep the plugin declaration and catalog data alive and unchanged.
std::expected<ResolvedPatch, Diagnostic> resolve_patch(const AdmittedPlugin &plugin, std::size_t patch_index,
                                                       std::span<const ImageProfile> catalog);

// Inspects the process executable directly and probes distinct candidate DLL hints, retaining pinned matches.
// catalog must be the same instance used to create the resolved patches.
LiveImageObservations observe_loaded_images(std::span<const ResolvedPatch *const> patches, std::span<const ImageProfile> catalog);

// Projects live results for one configuration or selection pass, preserving order, errors and executable classification.
// Reuse the result across the pass; profiles borrow the live records' catalog.
std::vector<ImageObservation> image_observations(std::span<const LiveImageObservation> images);

// Evaluates all patch variants for FC_ROLE_CLIENT or FC_ROLE_SERVER, with Ready taking precedence over Waiting.
// Present profiles must use the same catalog as patch; basename hints explain absent or failed candidates.
PatchSelection evaluate_patch(const ResolvedPatch &patch, FC_Role role, std::span<const ImageObservation> observations);

// Evaluates only the supplied variants from patch, using the same role and observation rules as above.
// Configuration evaluates one variant at a time to capture settings for Ready or Waiting variants.
PatchSelection evaluate_patch(const ResolvedPatch &patch, FC_Role role, std::span<const ImageObservation> observations,
                              std::span<const ResolvedVariant> variants);

} // namespace fc::runtime
