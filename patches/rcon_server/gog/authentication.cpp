#include "authentication.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <string>
#include <utility>

namespace fc::core_plugin::rcon_server::gog {
namespace {

// BCrypt uses nonnegative NTSTATUS values for successful operations.
bool bcrypt_succeeded(NTSTATUS status) noexcept {
    return status >= 0;
}

// Adds the authentication initialization context to a provider failure.
fc::Result<> authentication_error(std::string message) {
    return std::unexpected(fc::Error{"Could not initialize RCON authentication: " + message});
}

} // namespace

Authentication::~Authentication() {
    if (algorithm_ != nullptr) {
        BCryptCloseAlgorithmProvider(algorithm_, 0);
    }
}

fc::Result<> Authentication::prepare() {
    auto status = BCryptOpenAlgorithmProvider(&algorithm_, BCRYPT_MD5_ALGORITHM, nullptr, 0);
    if (!bcrypt_succeeded(status)) {
        return authentication_error("could not open the MD5 provider (status " + std::to_string(status) + ")");
    }

    // BCrypt determines the storage needed to construct a hash using this provider.
    DWORD result_size{};
    DWORD hash_object_size{};
    status = BCryptGetProperty(algorithm_, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&hash_object_size), sizeof(hash_object_size),
                               &result_size, 0);
    if (!bcrypt_succeeded(status)) {
        return authentication_error("could not read the MD5 hash object size (status " + std::to_string(status) + ")");
    }

    hash_object_.resize(hash_object_size);
    return {};
}

bool Authentication::matches(std::string_view password, std::string_view received_digest) noexcept {
    // Each attempt creates a hash in the reusable storage and destroys it before that storage is reused.
    BCRYPT_HASH_HANDLE hash{};
    auto status = BCryptCreateHash(algorithm_, &hash, hash_object_.data(), static_cast<ULONG>(hash_object_.size()), nullptr, 0, 0);
    if (!bcrypt_succeeded(status)) {
        return false;
    }

    status = BCryptHashData(hash, reinterpret_cast<PUCHAR>(const_cast<char *>(password.data())), static_cast<ULONG>(password.size()), 0);
    if (bcrypt_succeeded(status)) {
        status = BCryptFinishHash(hash, digest_.data(), static_cast<ULONG>(digest_.size()), 0);
    }
    BCryptDestroyHash(hash);
    if (!bcrypt_succeeded(status)) {
        return false;
    }

    // Compare all 32 hexadecimal characters, accumulating differences across the complete digest.
    constexpr char kHexDigits[] = "0123456789abcdef";
    std::uint8_t difference{};
    for (std::size_t index = 0; index < digest_.size(); ++index) {
        difference |= static_cast<std::uint8_t>(received_digest[index * 2] - kHexDigits[digest_[index] >> 4]);
        difference |= static_cast<std::uint8_t>(received_digest[index * 2 + 1] - kHexDigits[digest_[index] & 0x0f]);
    }
    return difference == 0;
}

} // namespace fc::core_plugin::rcon_server::gog
