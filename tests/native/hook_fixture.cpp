#include <cstdarg>
#include <cstdint>

// Exercises the compiler's ABI for an eight-byte record passed and returned by value.
struct NativeSmall {
    std::uint32_t first, second;
};

// Exercises a record large enough to require indirect return storage on the supported compiler ABIs.
struct NativeLarge {
    std::uint64_t first, second, third;
};

#if defined(_M_IX86)
// Export undecorated names so both architectures use the same GetProcAddress lookups.
#pragma comment(linker, "/export:fc_native_add=_fc_native_add")
#pragma comment(linker, "/export:fc_native_small=_fc_native_small")
#pragma comment(linker, "/export:fc_native_large=_fc_native_large")
#pragma comment(linker, "/export:fc_native_variadic=_fc_native_variadic")
#endif

// Returns distinct arithmetic on both arguments so hook tests can recognize the original result.
extern "C" __declspec(dllexport) __declspec(noinline) int __cdecl fc_native_add(int left, int right) noexcept {
    return left * 3 + right;
}

// Adjusts both fields of a by-value record to check compiler argument and return handling.
extern "C" __declspec(dllexport) __declspec(noinline) NativeSmall __cdecl fc_native_small(NativeSmall value) noexcept {
    return {value.first + 7, value.second + 9};
}

// Adjusts a large by-value record to exercise argument copying and indirect return storage.
extern "C" __declspec(dllexport) __declspec(noinline) NativeLarge __cdecl fc_native_large(NativeLarge value, int add) noexcept {
    return {value.first + static_cast<std::uint64_t>(add), value.second + static_cast<std::uint64_t>(add),
            value.third + static_cast<std::uint64_t>(add)};
}

// Sums count trailing int arguments through the compiler's variadic ABI.
extern "C" __declspec(dllexport) __declspec(noinline) int __cdecl fc_native_variadic(int count, ...) noexcept {
    // The compiler's va_list handles the architecture-specific trailing argument layout.
    va_list arguments;
    va_start(arguments, count);
    int result{};
    while (count-- > 0) {
        result += va_arg(arguments, int);
    }

    va_end(arguments);
    return result;
}
