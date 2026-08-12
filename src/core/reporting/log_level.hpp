#pragma once

#include <FusionCutter/reporting.hpp>

namespace fusioncutter::reporting_detail {

[[nodiscard]] constexpr LogLevel default_log_level() noexcept {
#if defined(_DEBUG)
    return LogLevel::Debug;
#else
    return LogLevel::Error;
#endif
}

} // namespace fusioncutter::reporting_detail
