#pragma once

#include <FusionCutter/outcome.hpp>

#include <Windows.h>
#include <bcrypt.h>

#include <array>
#include <expected>
#include <string_view>
#include <vector>

namespace fusioncutter::patches::rcon_server::gog {

// Verifies RCON client login digests against the game's current admin password.
class Authentication {
  public:
    Authentication() = default;
    ~Authentication();

    Authentication(const Authentication&) = delete;
    Authentication& operator=(const Authentication&) = delete;

    // Prepares the reusable BCrypt state required for client logins.
    [[nodiscard]] std::expected<void, OutcomeReason> prepare();

    // Hashes the supplied password and compares it with a wire-format digest.
    [[nodiscard]] bool matches(std::string_view password, std::string_view received_hash) noexcept;

  private:
    BCRYPT_ALG_HANDLE algorithm_{};
    DWORD hash_object_size_{};
    std::vector<UCHAR> hash_object_;
    std::array<UCHAR, 16> digest_{};
};

} // namespace fusioncutter::patches::rcon_server::gog
