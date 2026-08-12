#include <FusionCutter/environment.hpp>

#include <Windows.h>

#include <array>
#include <string>
#include <utility>

namespace fusioncutter::environment_detail {
namespace {

constexpr std::size_t kMaximumEnvironmentValue = 4096;

[[nodiscard]] OutcomeReason environment_error(std::string_view name, std::string message) {
    return {std::move(message), "Read environment variable '" + std::string(name) + "'", {}};
}

} // namespace

std::expected<std::optional<std::string>, OutcomeReason> read_variable(std::string_view name) {
    if (name.empty() || name.contains('\0') || name.contains('=')) {
        return std::unexpected(environment_error(name, "environment variable name is invalid"));
    }

    const std::string terminated_name(name);
    std::array<char, kMaximumEnvironmentValue + 1> value{};
    SetLastError(ERROR_SUCCESS);
    const auto length =
        GetEnvironmentVariableA(terminated_name.c_str(), value.data(), static_cast<DWORD>(value.size()));
    if (length == 0) {
        const auto error = GetLastError();
        if (error == ERROR_ENVVAR_NOT_FOUND) {
            return std::optional<std::string>{};
        }
        if (error == ERROR_SUCCESS) {
            return std::optional<std::string>{std::string{}};
        }
        return std::unexpected(environment_error(name, "environment variable could not be read (Windows error " +
                                                           std::to_string(error) + ")"));
    }
    if (length >= value.size()) {
        return std::unexpected(environment_error(name, "environment variable exceeds 4096 bytes"));
    }
    return std::optional<std::string>{std::string(value.data(), length)};
}

} // namespace fusioncutter::environment_detail
