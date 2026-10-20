#include "core_runtime.hpp"

#include <fc/abi.h>

#include <cstddef>
#include <exception>
#include <string>

namespace {

// Returns startup failure after synchronously sending its reason to the optional error sink.
std::uint32_t reject(FC_ErrorSink error, const char *reason) noexcept {
    if (error.write) {
        const auto size = static_cast<std::uint32_t>(std::char_traits<char>::length(reason));
        error.write(error.context, {reason, size}, 0);
    }
    return 0;
}

} // namespace

// Call outside loader lock; validates the request before waiting for control-thread initialization.
// Returns 1 on success or 0 with an optional synchronous error; the sink must copy any retained message.
extern "C" std::uint32_t FC_CDECL FC_Start(std::uint32_t abi, const FC_StartOptions *options, FC_ErrorSink error) noexcept {
    if (abi != FC_ABI_VERSION) {
        return reject(error, "FC_Start ABI version does not match this runtime");
    }
    if (!options || options->byte_size < offsetof(FC_StartOptions, dedicated) + sizeof(options->dedicated)) {
        return reject(error, "FC_Start options are missing or too small to contain dedicated");
    }
    if (options->dedicated > 1) {
        return reject(error, "FC_Start's dedicated option must be 0 or 1");
    }

    try {
        return fc::runtime::start_runtime(options->dedicated ? FC_ROLE_SERVER : FC_ROLE_CLIENT, error);
    } catch (const std::exception &exception) {
        return reject(error, exception.what());
    } catch (...) {
        return reject(error, "Runtime startup failed unexpectedly");
    }
}
