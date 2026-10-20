#pragma once

#include <fc/plugin.hpp>

#include <bcrypt.h>

#include <array>
#include <string_view>
#include <vector>

namespace fc::core_plugin::rcon_server::gog {

// Verifies legacy client login digests against the game's current administrator password.
class Authentication {
  public:
    Authentication() = default;

    // Releases the MD5 provider acquired during preparation.
    ~Authentication();

    Authentication(const Authentication &) = delete;
    Authentication &operator=(const Authentication &) = delete;

    // Acquires reusable Windows MD5 state before the listener can accept clients.
    fc::Result<> prepare();

    // Compares the password's MD5 with a lowercase hexadecimal digest; hash failures return false.
    // Supply exactly 32 digest characters and call serially after successful preparation to reuse hash storage safely.
    [[nodiscard]] bool matches(std::string_view password, std::string_view received_digest) noexcept;

  private:
    BCRYPT_ALG_HANDLE algorithm_{};
    // Reused by sequential authentication attempts on the service worker.
    std::vector<UCHAR> hash_object_;
    std::array<UCHAR, 16> digest_{};
};

} // namespace fc::core_plugin::rcon_server::gog
