#pragma once

#include <fc/reporting.hpp>

#include <type_traits>

namespace fc::core_plugin::rcon_server {

// Returns a prepared native operation's result; failure stops the process because RCON is required.
template <class T> T native_result(fc::Result<T> result) {
    if (!result) {
        fc::fatal(result.error());
    }
    if constexpr (!std::is_void_v<T>) {
        return *result;
    }
}

} // namespace fc::core_plugin::rcon_server
