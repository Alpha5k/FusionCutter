#pragma once

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>

namespace fusioncutter::reporting::detail {

class BoundedWriter {
  public:
    BoundedWriter(std::span<char> output, std::string_view truncation_marker) noexcept
        : output_(output), truncation_marker_(truncation_marker) {}

    void append(std::string_view text) noexcept {
        if (truncated_ || text.empty()) {
            return;
        }

        const auto marker_size = std::min(output_.size(), truncation_marker_.size());
        const auto limit = output_.size() - marker_size;
        const auto available = size_ < limit ? limit - size_ : 0;
        const auto copied = std::min(available, text.size());
        if (copied != 0) {
            std::memcpy(output_.data() + size_, text.data(), copied);
            size_ += copied;
        }
        truncated_ = copied != text.size();
    }

    void append_c_string(const char* text, std::size_t maximum_length = 256) noexcept {
        if (text == nullptr) {
            append("None");
            return;
        }

        std::size_t length = 0;
        while (length < maximum_length && text[length] != '\0') {
            ++length;
        }
        append({text, length});
        if (length == maximum_length) {
            append("...");
        }
    }

    void append_decimal(std::uint64_t value, std::size_t minimum_digits = 1) noexcept {
        append_integer(value, 10, minimum_digits);
    }

    void append_hex(std::uint64_t value, std::size_t minimum_digits = 1) noexcept {
        append_integer(value, 16, minimum_digits);
    }

    void append_pointer(std::uintptr_t value) noexcept {
        append("0x");
        append_hex(value, sizeof(std::uintptr_t) * 2);
    }

    [[nodiscard]] std::span<const char> finish() noexcept {
        if (truncated_) {
            const auto marker_size = std::min(output_.size() - size_, truncation_marker_.size());
            if (marker_size != 0) {
                std::memcpy(output_.data() + size_, truncation_marker_.data(), marker_size);
                size_ += marker_size;
            }
        }
        return {output_.data(), size_};
    }

  private:
    void append_integer(std::uint64_t value, int base, std::size_t minimum_digits) noexcept {
        constexpr std::string_view kZeroes = "00000000000000000000";
        std::array<char, 20> digits{};
        const auto [end, error] = std::to_chars(digits.data(), digits.data() + digits.size(), value, base);
        if (error != std::errc{}) {
            return;
        }

        std::string_view formatted{digits.data(), end};
        if (base == 16) {
            std::ranges::transform(formatted, digits.begin(), [](char digit) {
                return digit >= 'a' && digit <= 'f' ? static_cast<char>(digit - ('a' - 'A')) : digit;
            });
        }
        const auto width = std::min(minimum_digits, kZeroes.size());
        const auto padding = formatted.size() < width ? width - formatted.size() : 0;
        append(kZeroes.substr(0, padding));
        append(formatted);
    }

    std::span<char> output_;
    std::string_view truncation_marker_;
    std::size_t size_{};
    bool truncated_{};
};

} // namespace fusioncutter::reporting::detail
