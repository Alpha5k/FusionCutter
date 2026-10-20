#include "selection.hpp"

#include <fc/base.hpp>

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <iterator>
#include <ranges>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace fc::runtime {

namespace {

// Tests an image ID prefix using the same ASCII case-insensitive comparison as exact selectors.
bool starts_with_ascii_case_insensitive(std::string_view value, std::string_view prefix) {
    return value.size() >= prefix.size() && fc::detail::equal_id(value.substr(0, prefix.size()), prefix);
}

// Lists the original selectors for diagnostics when expansion yields no catalog profiles.
std::string declared_selectors(const FC_ImageRequirement &requirement) {
    std::string result;
    for (std::size_t index = 0; index < requirement.selector_count; ++index) {
        if (!result.empty()) {
            result += ", ";
        }
        result += "'" + std::string{fc::detail::view(requirement.selectors[index])} + "'";
    }

    return result;
}

// Combines the admitted plugin and local patch IDs into the diagnostic owner name.
std::string qualified_patch_id(const ResolvedPatch &patch) {
    return std::string{fc::detail::view(patch.plugin->id)} + "." + std::string{fc::detail::view(patch.definition->id)};
}

// Checks whether a variant's role mask includes the concrete client or server launch role.
bool role_matches(std::uint32_t declared, FC_Role current) {
    return (declared & static_cast<std::uint32_t>(current)) != 0;
}

// Associates a catalog filename hint with a profile during declaration overlap checks.
struct FilenameAssignment {
    std::string_view filename;
    // Every requirement choosing this basename must agree on this catalog profile.
    const ImageProfile *profile{};
};

// Finds an overlap using catalog hints. Renamed images can also overlap, so evaluation checks actual matches.
bool has_compatible_assignment(std::span<const ResolvedImageRequirement *const> requirements, std::size_t index,
                               std::vector<FilenameAssignment> &assignments) {
    if (index == requirements.size()) {
        return true;
    }

    const auto &requirement = *requirements[index];

    // Try each build under its hints, sharing a hint only when both requirements accept the same build.
    for (const auto *profile : requirement.profiles) {
        for (const auto filename : profile->filenames) {
            const auto existing = std::ranges::find_if(assignments, [&](const auto &assignment) {
                return fc::detail::equal_id(assignment.filename, filename);
            });
            if (existing != assignments.end()) {
                // Reuse an earlier filename choice only when both requirements accept the same build.
                if (existing->profile == profile && has_compatible_assignment(requirements, index + 1, assignments)) {
                    return true;
                }
                continue;
            }

            // Try this filename/profile pair, then check whether the remaining requirements can also be satisfied.
            assignments.push_back({filename, profile});
            if (has_compatible_assignment(requirements, index + 1, assignments)) {
                return true;
            }

            // Remove the tentative basename before trying the next profile or alias.
            assignments.pop_back();
        }
    }

    return false;
}

// Checks whether catalog hints already establish overlapping variants in a shared launch role.
bool variants_overlap(const ResolvedVariant &left, const ResolvedVariant &right) {
    if ((left.definition->role & right.definition->role) == 0) {
        return false;
    }

    // Combine both variants' requirements so the search checks whether all can be satisfied together.
    std::vector<const ResolvedImageRequirement *> requirements;
    requirements.reserve(left.images.size() + right.images.size());
    for (const auto &image : left.images) {
        if (image.profiles.empty()) {
            return false;
        }
        requirements.push_back(&image);
    }

    for (const auto &image : right.images) {
        if (image.profiles.empty()) {
            return false;
        }
        requirements.push_back(&image);
    }

    std::vector<FilenameAssignment> assignments;
    assignments.reserve(requirements.size());
    return has_compatible_assignment(requirements, 0, assignments);
}

// Checks whether the declared profiles can be selected together using their catalog filenames.
bool variant_can_match(const ResolvedVariant &variant) {
    std::vector<const ResolvedImageRequirement *> requirements;
    requirements.reserve(variant.images.size());
    for (const auto &image : variant.images) {
        if (image.profiles.empty()) {
            return false;
        }
        requirements.push_back(&image);
    }

    std::vector<FilenameAssignment> assignments;
    assignments.reserve(requirements.size());
    return has_compatible_assignment(requirements, 0, assignments);
}

// Intermediate state and diagnostics for one image label in one variant.
struct RequirementEvaluation {
    SelectionState state{SelectionState::Unavailable};
    // Set only when exactly one observed image satisfies this label.
    std::optional<SelectedImageBinding> binding;
    std::vector<Diagnostic> reasons;
    bool build_excluded{};
};

// Matches present profiles independently of basenames, then uses discovery hints to explain missing matches.
RequirementEvaluation evaluate_requirement(const ResolvedPatch &patch, const ResolvedImageRequirement &requirement,
                                           std::span<const ImageObservation> observations,
                                           const std::unordered_map<std::string, std::size_t> &observation_indices) {
    RequirementEvaluation result;
    const auto label = fc::detail::view(requirement.definition->name);
    const auto owner = qualified_patch_id(patch);

    std::vector<const ImageProfile *> pending_candidates;
    std::vector<std::size_t> matching_observations;
    std::vector<Diagnostic> unavailable_reasons;
    bool only_build_mismatches = true;

    // Present contents take precedence over absent hints or failed inspections at other aliases.
    for (const auto *candidate : requirement.profiles) {
        bool candidate_failed{};
        bool candidate_matched{};
        std::vector<Diagnostic> candidate_reasons;
        for (std::size_t index = 0; index != observations.size(); ++index) {
            const auto &observation = observations[index];
            if (!observation.outcome || !*observation.outcome || **observation.outcome != candidate) {
                continue;
            }
            candidate_matched = true;
            const bool already_matched = std::ranges::any_of(matching_observations, [&](auto previous) {
                return previous == index || (observation.base && observations[previous].base == observation.base);
            });
            if (!already_matched) {
                matching_observations.push_back(index);
            }
        }
        if (candidate_matched) {
            continue;
        }

        // An executable requirement refers to the process's actual main module, including failed inspections.
        const auto main = std::ranges::find(observations, true, &ImageObservation::executable);
        const bool expects_main = std::ranges::any_of(candidate->filenames, [](auto filename) {
            return filename.size() >= 4 && fc::detail::equal_id(filename.substr(filename.size() - 4), ".exe");
        });
        if (expects_main && main != observations.end()) {
            if (!main->outcome) {
                only_build_mismatches = false;
                auto diagnostic = main->outcome.error();
                diagnostic.message =
                    "image '" + std::string{label} + "' executable '" + main->filename + "' failed inspection: " + diagnostic.message;
                unavailable_reasons.push_back(std::move(diagnostic));
            } else {
                unavailable_reasons.push_back(
                    {owner, "image '" + std::string{label} + "' expected profile '" + std::string{candidate->id} + "' for executable '" +
                                main->filename + "' but observed " +
                                (*main->outcome ? "'" + std::string{(**main->outcome)->id} + "'" : "no recognized image")});
            }
            continue;
        }

        for (const auto filename : candidate->filenames) {
            const auto found = observation_indices.find(fc::detail::folded_id(filename));
            if (found == observation_indices.end()) {
                if (filename.size() >= 4 && fc::detail::equal_id(filename.substr(filename.size() - 4), ".exe")) {
                    candidate_failed = true;
                    candidate_reasons.push_back({owner, "image '" + std::string{label} + "' candidate '" + std::string{candidate->id} +
                                                            "' executable is unavailable"});
                }
                continue;
            }
            const auto &observation = observations[found->second];

            // Retain inspection errors with the label, candidate build, and filename that encountered them.
            if (!observation.outcome) {
                only_build_mismatches = false;
                auto diagnostic = observation.outcome.error();
                diagnostic.message = "image '" + std::string{label} + "' candidate '" + std::string{candidate->id} + "' at '" +
                                     observation.filename + "' failed inspection: " + diagnostic.message;
                candidate_reasons.push_back(std::move(diagnostic));
                candidate_failed = true;
                continue;
            }

            // Executables cannot arrive later; absent DLL aliases remain possible.
            if (!*observation.outcome) {
                if (filename.size() >= 4 && fc::detail::equal_id(filename.substr(filename.size() - 4), ".exe")) {
                    candidate_failed = true;
                    candidate_reasons.push_back({owner, "image '" + std::string{label} + "' candidate '" + std::string{candidate->id} +
                                                            "' executable '" + observation.filename + "' is absent"});
                }
                continue;
            }

            const auto *observed_profile = **observation.outcome;
            candidate_failed = true;
            only_build_mismatches &= observation.executable;
            candidate_reasons.push_back({owner, "image '" + std::string{label} + "' expected profile '" + std::string{candidate->id} +
                                                    "' at '" + observation.filename + "' but observed '" +
                                                    std::string{observed_profile->id} + "'"});
        }

        // With no match, a failed or mismatched alias defeats the candidate; otherwise it remains pending.
        if (candidate_failed) {
            unavailable_reasons.insert(unavailable_reasons.end(), std::make_move_iterator(candidate_reasons.begin()),
                                       std::make_move_iterator(candidate_reasons.end()));
        } else {
            pending_candidates.push_back(candidate);
        }
    }

    // One physical match binds the label even when other candidates failed or remain absent.
    if (matching_observations.size() == 1) {
        const auto index = matching_observations.front();
        result.state = SelectionState::Ready;
        result.binding = SelectedImageBinding{label, **observations[index].outcome, index};
        return result;
    }

    // Several matching observations are ambiguous, even if they identify the same build.
    if (matching_observations.size() > 1) {
        std::string names;
        for (const auto index : matching_observations) {
            if (!names.empty()) {
                names += ", ";
            }
            names += observations[index].filename;
        }
        result.reasons.push_back({owner, "image '" + std::string{label} + "' is ambiguous across observed images: " + names});
        return result;
    }

    // With no match, a candidate whose DLL aliases are all absent keeps the label waiting.
    if (!pending_candidates.empty()) {
        std::string pending;
        for (const auto *profile : pending_candidates) {
            if (!pending.empty()) {
                pending += "; ";
            }
            pending += "profile '" + std::string{profile->id} + "' at ";
            for (std::size_t index = 0; index < profile->filenames.size(); ++index) {
                if (index != 0) {
                    pending += ", ";
                }
                pending += "'" + std::string{profile->filenames[index]} + "'";
            }
        }

        result.state = SelectionState::Waiting;
        result.reasons.push_back({owner, "image '" + std::string{label} + "' is waiting for " + pending});
        return result;
    }

    // Every candidate is unusable, so report the failures collected across their aliases.
    result.reasons = std::move(unavailable_reasons);
    result.build_excluded = only_build_mismatches;

    return result;
}

} // namespace

std::expected<ResolvedPatch, Diagnostic> resolve_patch(const AdmittedPlugin &plugin, std::size_t patch_index,
                                                       std::span<const ImageProfile> catalog) {
    assert(plugin.definition && patch_index < plugin.definition->patch_count);

    ResolvedPatch result{plugin.owner, plugin.definition, &plugin.definition->patches[patch_index]};

    // Preserve declaration order and original variant indices while expanding each image requirement.
    result.variants.reserve(result.definition->variant_count);
    for (std::size_t variant_index = 0; variant_index < result.definition->variant_count; ++variant_index) {
        const auto &definition = result.definition->variants[variant_index];
        ResolvedVariant variant{&definition, variant_index};
        variant.images.reserve(definition.image_count);
        for (std::size_t image_index = 0; image_index < definition.image_count; ++image_index) {
            const auto &image = definition.images[image_index];
            ResolvedImageRequirement resolved{&image};

            // Visit each catalog record once so overlapping selectors preserve catalog order and uniqueness.
            for (const auto &profile : catalog) {
                const auto matches = std::ranges::any_of(std::span{image.selectors, image.selector_count}, [&](FC_Text selector_text) {
                    const auto selector = fc::detail::view(selector_text);

                    // Selectors compare IDs ASCII case-insensitively. A trailing '*' matches a prefix;
                    // a lone '*' accepts every profile.
                    return selector.ends_with('*') ? starts_with_ascii_case_insensitive(profile.id, selector.substr(0, selector.size() - 1))
                                                   : fc::detail::equal_id(profile.id, selector);
                });
                if (matches) {
                    resolved.profiles.push_back(&profile);
                }
            }

            variant.images.push_back(std::move(resolved));
        }

        result.variants.push_back(std::move(variant));
    }

    // Reject overlaps established by catalog hints; evaluation also rejects ambiguous observed matches.
    for (std::size_t left = 0; left < result.variants.size(); ++left) {
        for (std::size_t right = left + 1; right < result.variants.size(); ++right) {
            if (variants_overlap(result.variants[left], result.variants[right])) {
                return std::unexpected{Diagnostic{plugin.owner, "patch '" + qualified_patch_id(result) + "' variants " +
                                                                    std::to_string(left + 1) + " and " + std::to_string(right + 1) +
                                                                    " can match the same image set in the same launch role"}};
            }
        }
    }

    return result;
}

LiveImageObservations observe_loaded_images(std::span<const ResolvedPatch *const> patches, std::span<const ImageProfile> catalog) {
    std::vector<std::string> filenames;
    std::unordered_set<std::string> seen;

    // Collect DLL hints from every variant. The process executable is inspected directly below.
    for (const auto *patch : patches) {
        for (const auto &variant : patch->variants) {
            for (const auto &requirement : variant.images) {
                for (const auto *profile : requirement.profiles) {
                    for (const auto filename : profile->filenames) {
                        if (filename.size() >= 4 && fc::detail::equal_id(filename.substr(filename.size() - 4), ".exe")) {
                            continue;
                        }
                        const auto folded = fc::detail::folded_id(filename);
                        if (seen.insert(folded).second) {
                            filenames.emplace_back(filename);
                        }
                    }
                }
            }
        }
    }

    LiveImageObservations result;
    result.reserve(filenames.size());

    // Preserve first-declaration order so bindings keep their indices when later modules are appended.
    for (const auto &filename : filenames) {
        result.push_back({filename, probe_loaded_image(filename, catalog)});
    }

    // Windows supplies the main module independently of its filename or the patch declarations.
    auto main = probe_main_image(catalog);
    if (main) {
        auto filename = main->filename;
        result.push_back({std::move(filename), std::move(*main), true});
    } else {
        auto filename = main.error().owner;
        result.push_back({std::move(filename), std::unexpected{std::move(main.error())}, true});
    }

    return result;
}

std::vector<ImageObservation> image_observations(std::span<const LiveImageObservation> images) {
    std::vector<ImageObservation> result;
    result.reserve(images.size());
    for (const auto &image : images) {
        ImageObservationOutcome outcome = std::optional<const ImageProfile *>{};
        if (!image.outcome) {
            outcome = std::unexpected{image.outcome.error()};
        } else if (*image.outcome) {
            outcome = (*image.outcome)->profile;
        }
        const auto base = image.outcome && *image.outcome ? (*image.outcome)->base : 0;
        result.push_back({image.filename, std::move(outcome), image.executable, base});
    }

    return result;
}

PatchSelection evaluate_patch(const ResolvedPatch &patch, FC_Role role, std::span<const ImageObservation> observations,
                              std::span<const ResolvedVariant> variants) {
    const auto owner = qualified_patch_id(patch);
    assert(role == FC_ROLE_CLIENT || role == FC_ROLE_SERVER);

    // Map basenames to their original input positions so selected bindings can index the caller's image data.
    std::unordered_map<std::string, std::size_t> observation_indices;
    for (std::size_t index = 0; index < observations.size(); ++index) {
        observation_indices.emplace(fc::detail::folded_id(observations[index].filename), index);
    }

    std::vector<Diagnostic> waiting_reasons;
    std::vector<Diagnostic> unavailable_reasons;
    bool has_applicable_variant{};
    bool only_build_exclusions = true;
    std::optional<PatchSelection> ready;

    // Evaluate each variant allowed for this concrete launch role.
    for (const auto &variant : variants) {
        if (!role_matches(variant.definition->role, role)) {
            continue;
        }

        has_applicable_variant = true;

        // Preserve declaration feasibility for the named supplemental modules.
        if (!variant_can_match(variant)) {
            const auto empty = std::ranges::find_if(variant.images, [](const ResolvedImageRequirement &image) {
                return image.profiles.empty();
            });
            if (empty != variant.images.end()) {
                const auto label = fc::detail::view(empty->definition->name);
                unavailable_reasons.push_back({owner, "image '" + std::string{label} + "' selectors " +
                                                          declared_selectors(*empty->definition) +
                                                          " match no profiles in this runtime catalog"});
            } else {
                only_build_exclusions = false;
                unavailable_reasons.push_back(
                    {owner, "variant " + std::to_string(variant.index + 1) +
                                " requires image profiles that cannot be selected together using their declared filenames"});
            }
            continue;
        }

        // Gather bindings and diagnostics for every label before deciding the variant's state.
        PatchSelection candidate{SelectionState::Ready, variant.definition, variant.index};
        bool has_failure{};
        for (const auto &requirement : variant.images) {
            auto image = evaluate_requirement(patch, requirement, observations, observation_indices);
            if (image.state == SelectionState::Unavailable) {
                candidate.state = SelectionState::Unavailable;
                has_failure |= !image.build_excluded;
            } else if (image.state == SelectionState::Waiting && candidate.state == SelectionState::Ready) {
                candidate.state = SelectionState::Waiting;
            } else if (image.state == SelectionState::Ready) {
                candidate.images.push_back(*image.binding);
            }
            candidate.reasons.insert(candidate.reasons.end(), std::make_move_iterator(image.reasons.begin()),
                                     std::make_move_iterator(image.reasons.end()));
        }

        // One unavailable label defeats the variant; otherwise any pending label makes it wait.
        if (candidate.state == SelectionState::Unavailable) {
            only_build_exclusions &= !has_failure;
            unavailable_reasons.insert(unavailable_reasons.end(), std::make_move_iterator(candidate.reasons.begin()),
                                       std::make_move_iterator(candidate.reasons.end()));
        } else if (candidate.state == SelectionState::Waiting) {
            waiting_reasons.insert(waiting_reasons.end(), std::make_move_iterator(candidate.reasons.begin()),
                                   std::make_move_iterator(candidate.reasons.end()));
        } else {
            // Catalog hints cannot prove exclusivity after renaming; reject actual simultaneous matches.
            if (ready) {
                PatchSelection ambiguous;
                ambiguous.reasons.push_back({owner, "variants " + std::to_string(ready->variant_index + 1) + " and " +
                                                        std::to_string(candidate.variant_index + 1) +
                                                        " both match the observed images in this launch role"});
                return ambiguous;
            }
            ready = std::move(candidate);
        }
    }

    if (ready) {
        return std::move(*ready);
    }

    PatchSelection result;

    // With none ready, report Waiting if any evaluated variant is waiting; otherwise report unavailable reasons.
    if (!waiting_reasons.empty()) {
        result.state = SelectionState::Waiting;
        result.reasons = std::move(waiting_reasons);
    } else {
        result.state = SelectionState::Unavailable;
        result.not_applicable = !has_applicable_variant || only_build_exclusions;
        result.reasons = std::move(unavailable_reasons);
        if (!has_applicable_variant) {
            result.reasons.push_back({owner, "patch has no variant for the current launch role"});
        }
    }

    return result;
}

PatchSelection evaluate_patch(const ResolvedPatch &patch, FC_Role role, std::span<const ImageObservation> observations) {
    return evaluate_patch(patch, role, observations, patch.variants);
}

} // namespace fc::runtime
