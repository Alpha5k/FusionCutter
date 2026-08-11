#include "security.hpp"

#include <Windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstring>
#include <limits>

namespace fusioncutter::patches::direct_transport {
namespace {

constexpr std::size_t kConnectionIdAttempts = 128;

[[nodiscard]] std::uint64_t load_little_endian64(const std::uint8_t* bytes) noexcept {
    static_assert(std::endian::native == std::endian::little);
    std::uint64_t value{};
    std::memcpy(&value, bytes, sizeof(value));
    return value;
}

// Incremental SipHash-2-4 accepts separate authentication fields without concatenating packet buffers.
class SipHasher {
  public:
    explicit SipHasher(std::span<const std::uint8_t, 16> key) noexcept {
        const auto first = load_little_endian64(key.data());
        const auto second = load_little_endian64(key.data() + 8);
        v0_ = 0x736f6d6570736575ULL ^ first;
        v1_ = 0x646f72616e646f6dULL ^ second;
        v2_ = 0x6c7967656e657261ULL ^ first;
        v3_ = 0x7465646279746573ULL ^ second;
    }

    void update(std::span<const std::uint8_t> bytes) noexcept {
        total_bytes_ += bytes.size();
        if (tail_bytes_ != 0) {
            while (!bytes.empty() && tail_bytes_ < tail_.size()) {
                tail_[tail_bytes_++] = bytes.front();
                bytes = bytes.subspan(1);
            }
            if (tail_bytes_ == tail_.size()) {
                compress(load_little_endian64(tail_.data()));
                tail_bytes_ = 0;
            }
        }
        while (bytes.size() >= 8) {
            compress(load_little_endian64(bytes.data()));
            bytes = bytes.subspan(8);
        }
        while (!bytes.empty()) {
            tail_[tail_bytes_++] = bytes.front();
            bytes = bytes.subspan(1);
        }
    }

    [[nodiscard]] std::uint64_t finish() noexcept {
        std::uint64_t final_block = static_cast<std::uint64_t>(total_bytes_) << 56;
        for (std::size_t index = 0; index < tail_bytes_; ++index) {
            final_block |= static_cast<std::uint64_t>(tail_[index]) << (index * 8);
        }
        v3_ ^= final_block;
        run_rounds(2);
        v0_ ^= final_block;
        v2_ ^= 0xFF;
        run_rounds(4);
        return v0_ ^ v1_ ^ v2_ ^ v3_;
    }

  private:
    void round() noexcept {
        v0_ += v1_;
        v1_ = std::rotl(v1_, 13);
        v1_ ^= v0_;
        v0_ = std::rotl(v0_, 32);
        v2_ += v3_;
        v3_ = std::rotl(v3_, 16);
        v3_ ^= v2_;
        v0_ += v3_;
        v3_ = std::rotl(v3_, 21);
        v3_ ^= v0_;
        v2_ += v1_;
        v1_ = std::rotl(v1_, 17);
        v1_ ^= v2_;
        v2_ = std::rotl(v2_, 32);
    }

    void run_rounds(unsigned int count) noexcept {
        for (unsigned int round_index = 0; round_index < count; ++round_index) {
            round();
        }
    }

    void compress(std::uint64_t word) noexcept {
        v3_ ^= word;
        run_rounds(2);
        v0_ ^= word;
    }

    std::uint64_t v0_{};
    std::uint64_t v1_{};
    std::uint64_t v2_{};
    std::uint64_t v3_{};
    std::array<std::uint8_t, 8> tail_{};
    std::size_t tail_bytes_{};
    std::size_t total_bytes_{};
};

[[nodiscard]] bool system_random(void*, std::uint8_t* output, std::size_t bytes) noexcept {
    if (output == nullptr || bytes > (std::numeric_limits<ULONG>::max)()) {
        return false;
    }
    return BCryptGenRandom(nullptr, output, static_cast<ULONG>(bytes), BCRYPT_USE_SYSTEM_PREFERRED_RNG) >= 0;
}

} // namespace

RandomSource system_random_source() noexcept {
    return {nullptr, &system_random};
}

bool fill_random(const RandomSource& source, std::span<std::uint8_t> output) noexcept {
    return output.empty() || (source.fill != nullptr && source.fill(source.context, output.data(), output.size()));
}

bool generate_connection_id(const RandomSource& source, std::span<const std::uint32_t> live_ids,
                            std::uint32_t& output) noexcept {
    output = 0;
    for (std::size_t attempt = 0; attempt < kConnectionIdAttempts; ++attempt) {
        std::array<std::uint8_t, 4> bytes{};
        if (!fill_random(source, bytes)) {
            return false;
        }
        const auto candidate = load_big_endian32(bytes.data());
        if (candidate != 0 && !std::ranges::contains(live_ids, candidate)) {
            output = candidate;
            return true;
        }
    }
    return false;
}

std::uint64_t sip_hash24(std::span<const std::uint8_t, 16> key, std::span<const std::uint8_t> message) noexcept {
    SipHasher hasher(key);
    hasher.update(message);
    return hasher.finish();
}

std::uint64_t compute_authentication_tag(Direction direction, std::span<const std::uint8_t, 16> key,
                                         std::span<const std::uint8_t> serialized_header,
                                         std::span<const std::uint8_t> payload) noexcept {
    if (serialized_header.size() != kDirectHeaderBytes) {
        return 0;
    }

    std::array<std::uint8_t, kDirectHeaderBytes> authenticated_header{};
    std::ranges::copy(serialized_header, authenticated_header.begin());
    std::ranges::fill(std::span(authenticated_header).subspan(offsetof(DirectHeaderV1, authentication_tag)),
                      std::uint8_t{});
    const std::array direction_domain{static_cast<std::uint8_t>(direction)};

    SipHasher hasher(key);
    hasher.update(direction_domain);
    hasher.update(authenticated_header);
    hasher.update(payload);
    return hasher.finish();
}

bool verify_authentication_tag(Direction direction, std::span<const std::uint8_t, 16> key,
                               std::span<const std::uint8_t> serialized_header,
                               std::span<const std::uint8_t> payload) noexcept {
    if (serialized_header.size() != kDirectHeaderBytes) {
        return false;
    }
    std::array<std::uint8_t, 8> expected_tag{};
    store_big_endian64(expected_tag.data(), compute_authentication_tag(direction, key, serialized_header, payload));
    const auto received_tag =
        serialized_header.subspan(offsetof(DirectHeaderV1, authentication_tag), expected_tag.size());
    return constant_time_equal(expected_tag, received_tag);
}

bool constant_time_equal(std::span<const std::uint8_t> left, std::span<const std::uint8_t> right) noexcept {
    if (left.size() != right.size()) {
        return false;
    }
    // Authentication comparisons inspect every byte instead of returning after the first mismatch.
    std::uint8_t difference{};
    for (std::size_t index = 0; index < left.size(); ++index) {
        difference = static_cast<std::uint8_t>(difference | (left[index] ^ right[index]));
    }
    return difference == 0;
}

ReplayResult ReplayWindow::admit(std::uint32_t sequence) noexcept {
    if (!initialized_) {
        initialized_ = true;
        frontier_ = sequence;
        seen_ = {};
        seen_[0] = 1;
        return ReplayResult::AcceptedFirst;
    }

    const auto forward = sequence - frontier_;
    if (forward == 0) {
        return ReplayResult::Duplicate;
    }
    if (forward == 0x80000000U) {
        return ReplayResult::InvalidJump;
    }
    if (forward < 0x80000000U) {
        advance(forward);
        frontier_ = sequence;
        seen_[0] |= 1;
        return ReplayResult::AcceptedNew;
    }

    const auto behind = frontier_ - sequence;
    if (behind >= kWidth) {
        return ReplayResult::Stale;
    }
    const auto word = behind / 64;
    const auto bit = behind % 64;
    const auto mask = std::uint64_t{1} << bit;
    if ((seen_[word] & mask) != 0) {
        return ReplayResult::Duplicate;
    }
    seen_[word] |= mask;
    return ReplayResult::AcceptedReordered;
}

void ReplayWindow::reset() noexcept {
    seen_ = {};
    frontier_ = 0;
    initialized_ = false;
}

bool ReplayWindow::initialized() const noexcept {
    return initialized_;
}

std::uint32_t ReplayWindow::frontier() const noexcept {
    return frontier_;
}

void ReplayWindow::advance(std::uint32_t distance) noexcept {
    if (distance >= kWidth) {
        seen_ = {};
        return;
    }

    const auto words = distance / 64;
    const auto bits = distance % 64;
    std::array<std::uint64_t, 4> shifted{};
    for (std::size_t destination = shifted.size(); destination-- > 0;) {
        if (destination < words) {
            continue;
        }
        const auto source = destination - words;
        shifted[destination] |= seen_[source] << bits;
        if (bits != 0 && source > 0) {
            shifted[destination] |= seen_[source - 1] >> (64 - bits);
        }
    }
    seen_ = shifted;
}

} // namespace fusioncutter::patches::direct_transport
