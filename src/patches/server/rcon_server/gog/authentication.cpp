#include "authentication.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>

namespace fusioncutter::patches::rcon_server::gog {
namespace {

[[nodiscard]] OutcomeReason authentication_error(std::string message) {
    return {std::move(message), "Initialize RCON authentication", {}};
}

[[nodiscard]] bool bcrypt_succeeded(NTSTATUS status) noexcept {
    return status >= 0;
}

} // namespace

Authentication::~Authentication() {
    if (algorithm_ != nullptr) {
        BCryptCloseAlgorithmProvider(algorithm_, 0);
    }
}

std::expected<void, OutcomeReason> Authentication::prepare() {
    auto status = BCryptOpenAlgorithmProvider(&algorithm_, BCRYPT_MD5_ALGORITHM, nullptr, 0);
    if (!bcrypt_succeeded(status)) {
        return std::unexpected(
            authentication_error("BCrypt could not open MD5 provider (status " + std::to_string(status) + ")"));
    }

    DWORD result_size{};
    DWORD hash_size{};
    status = BCryptGetProperty(algorithm_, BCRYPT_HASH_LENGTH, reinterpret_cast<PUCHAR>(&hash_size), sizeof(hash_size),
                               &result_size, 0);
    if (!bcrypt_succeeded(status) || hash_size != digest_.size()) {
        return std::unexpected(authentication_error("BCrypt returned an incompatible MD5 hash size"));
    }

    status = BCryptGetProperty(algorithm_, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&hash_object_size_),
                               sizeof(hash_object_size_), &result_size, 0);
    if (!bcrypt_succeeded(status) || hash_object_size_ == 0) {
        return std::unexpected(authentication_error("BCrypt could not determine its MD5 object size"));
    }
    hash_object_.resize(hash_object_size_);
    return {};
}

bool Authentication::matches(std::string_view password, std::string_view received_hash) noexcept {
    if (algorithm_ == nullptr || received_hash.size() != digest_.size() * 2) {
        return false;
    }

    BCRYPT_HASH_HANDLE hash{};
    auto status = BCryptCreateHash(algorithm_, &hash, hash_object_.data(), hash_object_size_, nullptr, 0, 0);
    if (!bcrypt_succeeded(status)) {
        return false;
    }

    status = BCryptHashData(hash, reinterpret_cast<PUCHAR>(const_cast<char*>(password.data())),
                            static_cast<ULONG>(password.size()), 0);
    if (bcrypt_succeeded(status)) {
        status = BCryptFinishHash(hash, digest_.data(), static_cast<ULONG>(digest_.size()), 0);
    }
    BCryptDestroyHash(hash);
    if (!bcrypt_succeeded(status)) {
        return false;
    }

    // Compare the complete lowercase hexadecimal digest instead of stopping at its first differing byte.
    constexpr char kHex[] = "0123456789abcdef";
    std::uint8_t difference{};
    for (std::size_t index = 0; index < digest_.size(); ++index) {
        difference |= static_cast<std::uint8_t>(received_hash[index * 2] - kHex[digest_[index] >> 4]);
        difference |= static_cast<std::uint8_t>(received_hash[index * 2 + 1] - kHex[digest_[index] & 0x0F]);
    }
    return difference == 0;
}

} // namespace fusioncutter::patches::rcon_server::gog
