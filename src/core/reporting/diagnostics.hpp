#pragma once

#include <cstdint>

namespace fusioncutter::diagnostics_detail {

void configure(std::uint32_t maximum_file_size_mb) noexcept;

} // namespace fusioncutter::diagnostics_detail
