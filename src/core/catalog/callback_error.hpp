#pragma once

#include <FusionCutter/PluginApi.h>

#include <string>
#include <string_view>

namespace fc::catalog {

// Copies the first plugin failure into framework storage so borrowed callback text remains safe to report.
struct CallbackError {
    std::string message;
    std::string operation;
    std::string_view malformed_fallback{"The callback supplied malformed error text"};
    std::string_view failure_fallback{"The callback failed"};
    bool supplied{};

    [[nodiscard]] FC_ErrorSink sink() noexcept;
};

} // namespace fc::catalog
