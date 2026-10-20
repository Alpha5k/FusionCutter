#pragma once

#include "patch_record.hpp"

namespace fc::runtime {

// Checks again for DLLs needed by waiting patches or runtime integrations, then updates patch selection on the control thread.
// The observations must use this session's catalog. Returns changed indices and keeps recognized modules loaded until exit.
std::vector<std::size_t> process_awaited_images(SelectionSession &, LiveImageObservations &,
                                                std::span<const std::string_view> private_filenames = {});

} // namespace fc::runtime
