#include "callback_error.hpp"

#include "catalog_types.hpp"
#include "definition_copy.hpp"

#include <algorithm>

namespace fc::catalog {
namespace {

// Truncation preserves UTF-8 boundaries while sharing one budget between message and operation.
[[nodiscard]] std::size_t utf8_prefix(std::string_view value, std::size_t capacity) noexcept {
    auto size = std::min(value.size(), capacity);
    if (size == value.size()) {
        return size;
    }
    while (size != 0 && (static_cast<unsigned char>(value[size]) & 0xc0U) == 0x80U) {
        --size;
    }
    return size;
}

void FC_CALL capture_error(void* context, FC_StringView message, FC_StringView operation) noexcept {
    if (context == nullptr) {
        return;
    }
    auto& error = *static_cast<CallbackError*>(context);
    // Only the first plugin report is authoritative; later cleanup or wrapper errors cannot hide the root failure.
    if (error.supplied) {
        return;
    }
    error.supplied = true;
    try {
        // Validate borrowed views before copying, then share one bounded budget across both diagnostic fields.
        if ((message.size != 0 && message.data == nullptr) || (operation.size != 0 && operation.data == nullptr)) {
            error.message = error.malformed_fallback;
            return;
        }
        const std::string_view message_view{message.size == 0 ? "" : message.data, message.size};
        const std::string_view operation_view{operation.size == 0 ? "" : operation.data, operation.size};
        if (!valid_utf8(message_view) || !valid_utf8(operation_view)) {
            error.message = error.failure_fallback;
            return;
        }
        const auto message_size = utf8_prefix(message_view, kCallbackErrorByteCapacity);
        error.message.assign(message_view.data(), message_size);
        const auto operation_size = utf8_prefix(operation_view, kCallbackErrorByteCapacity - message_size);
        error.operation.assign(operation_view.data(), operation_size);
    } catch (...) {
        // Diagnostic allocation failure degrades to the caller's ordinary phase fallback without crossing the ABI.
        error.message.clear();
        error.operation.clear();
    }
}

} // namespace

FC_ErrorSink CallbackError::sink() noexcept {
    return {.struct_size = sizeof(FC_ErrorSink), .context = this, .set = &capture_error};
}

} // namespace fc::catalog
