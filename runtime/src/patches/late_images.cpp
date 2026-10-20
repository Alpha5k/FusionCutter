#include "late_images.hpp"

#include <fc/base.hpp>

#include <algorithm>
#include <utility>

namespace fc::runtime {

namespace {

// Reports whether a waiting patch has a variant for the session's role that accepts this filename.
bool required_filename(const SelectionSession &session, std::string_view filename) {
    for (const auto &patch : session.patches) {
        if (patch->state != PatchState::Waiting) {
            continue;
        }

        for (const auto &variant : patch->resolved.variants) {
            if (!(variant.definition->role & session.role)) {
                continue;
            }

            for (const auto &requirement : variant.images) {
                for (const auto *profile : requirement.profiles) {
                    if (std::ranges::any_of(profile->filenames, [&](auto value) {
                            return fc::detail::equal_id(value, filename);
                        })) {
                        return true;
                    }
                }
            }
        }
    }

    return false;
}

} // namespace

std::vector<std::size_t> process_awaited_images(SelectionSession &session, LiveImageObservations &images,
                                                std::span<const std::string_view> private_filenames) {
    if (private_filenames.empty() && std::ranges::none_of(session.patches, [](const auto &patch) {
            return patch->state == PatchState::Waiting;
        })) {
        return {};
    }

    // Retry only images previously recorded as absent; recognized images and recognition failures are final.
    std::vector<std::size_t> changed;
    for (std::size_t i = 0; i != images.size(); ++i) {
        auto &observation = images[i];
        const bool private_required = std::ranges::any_of(private_filenames, [&](auto filename) {
            return fc::detail::equal_id(filename, observation.filename);
        });
        if (!observation.outcome || *observation.outcome || (!private_required && !required_filename(session, observation.filename))) {
            continue;
        }

        auto result = probe_loaded_image(observation.filename, session.catalog);
        if (!result || *result) {
            observation.outcome = std::move(result);
            changed.push_back(i);
        }
    }

    if (!changed.empty()) {
        const auto observations = image_observations(images);
        refresh_selection(session, observations);
    }

    return changed;
}

} // namespace fc::runtime
