#pragma once

namespace fc {

namespace detail {

// Composition validation keeps malformed author models from reaching the native registration boundary.
[[noreturn]] inline void invalid_composition(std::string_view message) {
    throw std::invalid_argument{std::string{message}};
}

inline void require_id(std::string_view value, std::string_view subject) {
    if (!valid_id(value)) {
        throw std::invalid_argument{std::string{subject} +
                                    " must contain 1-64 ASCII letters, digits, or underscores and begin with a letter"};
    }
}

inline void validate_relationships(const std::vector<std::string>& relationships) {
    for (const auto& relationship : relationships) {
        require_id(relationship, "A relationship ID");
    }
}

// Patch, group, and category IDs additionally reserve General because those IDs can become complete INI sections.
[[nodiscard]] constexpr bool reserved_catalog_id(std::string_view value) noexcept {
    return reserved_plugin_id(value) || equal_ascii_case_insensitive(value, "General");
}

[[nodiscard]] constexpr bool valid_setting_key(std::string_view value) noexcept {
    if (value.empty() || value.front() == ';' || value.front() == '#' || value.front() == ' ' ||
        value.front() == '\t' || value.back() == ' ' || value.back() == '\t') {
        return false;
    }
    return std::ranges::all_of(value, [](char character) {
        const auto byte = static_cast<unsigned char>(character);
        return byte <= 0x7f && byte != 0 && byte != '\r' && byte != '\n' && byte != '=' && byte != ':';
    });
}

[[nodiscard]] constexpr bool valid_environment_name(std::string_view value) noexcept {
    if (value.empty()) {
        return false;
    }
    return std::ranges::all_of(value, [](char character) {
        const auto byte = static_cast<unsigned char>(character);
        return (byte >= 'A' && byte <= 'Z') || (byte >= 'a' && byte <= 'z') || (byte >= '0' && byte <= '9') ||
               byte == '_';
    });
}

template <class Settings>
void validate_setting_entries(const std::vector<SettingEntry<Settings>>& entries, std::string_view section) {
    for (std::size_t index = 0; index < entries.size(); ++index) {
        const auto& data = SettingEntryAccess<Settings>::data(entries[index]);
        if (!data) {
            invalid_composition("A settings schema contains an empty setting entry");
        }
        if (!valid_setting_key(data->key)) {
            invalid_composition("A setting key does not satisfy the Fusion Cutter INI syntax");
        }
        if (!data->environment.empty() && !valid_environment_name(data->environment)) {
            invalid_composition("A setting environment override must contain ASCII letters, digits, or underscores");
        }
        // Duplicate IDs are checked per section without ASCII case distinctions, matching configuration lookup.
        for (std::size_t prior = 0; prior < index; ++prior) {
            const auto& prior_data = SettingEntryAccess<Settings>::data(entries[prior]);
            if (equal_ascii_case_insensitive(prior_data->key, data->key)) {
                throw std::invalid_argument{"A settings schema repeats key '" + data->key + "' in section '" +
                                            std::string{section} + "'"};
            }
        }
        // Recheck builder-owned constraints because default-constructed or manually composed entries may reach here.
        if (data->type == FC_SETTING_STRING && data->max_length != 0 &&
            data->default_string.size() > data->max_length) {
            invalid_composition("A string setting default exceeds its declared maximum length");
        }
        if (data->type == FC_SETTING_CHOICE && data->choice_names.empty()) {
            invalid_composition("A choice setting must declare at least one choice");
        }
    }
}

template <class Settings> void validate_settings_schema(const SettingsSchema<Settings>& schema) {
    validate_setting_entries(schema.entries, "");
    for (std::size_t index = 0; index < schema.sections.size(); ++index) {
        const auto& section = schema.sections[index];
        require_id(section.id, "A settings section ID");
        for (std::size_t prior = 0; prior < index; ++prior) {
            if (equal_ascii_case_insensitive(schema.sections[prior].id, section.id)) {
                invalid_composition("A settings schema cannot repeat a section ID");
            }
        }
        validate_setting_entries(section.entries, section.id);
    }
}

[[nodiscard]] constexpr bool valid_layout(TargetLayout layout) noexcept {
    switch (layout) {
    case TargetLayout::GameSpyRetail:
    case TargetLayout::SteamRetail:
    case TargetLayout::GOGRetail:
    case TargetLayout::ModTools:
    case TargetLayout::ClassicCollection:
        return true;
    }
    return false;
}

[[nodiscard]] constexpr bool valid_role_mask(HostRole roles) noexcept {
    return roles == HostRole::Client || roles == HostRole::Server || roles == HostRole::All;
}

[[nodiscard]] constexpr bool valid_image(TargetImage image) noexcept {
    return image == TargetImage::Game || image == TargetImage::Bootstrap || image == TargetImage::GalaxyPeer;
}

[[nodiscard]] constexpr bool valid_failure_policy(FailurePolicy policy) noexcept {
    return policy == FailurePolicy::Continue || policy == FailurePolicy::Fatal;
}

template <class Settings> void validate_support_definition(const SupportDefinition<Settings>& definition) {
    if (definition.layouts.empty()) {
        invalid_composition("A support must declare at least one target layout");
    }
    // Layouts form a set even though author composition preserves their declaration order in a vector.
    for (std::size_t index = 0; index < definition.layouts.size(); ++index) {
        if (!valid_layout(definition.layouts[index])) {
            invalid_composition("A support contains an invalid target layout");
        }
        for (std::size_t prior = 0; prior < index; ++prior) {
            if (definition.layouts[prior] == definition.layouts[index]) {
                invalid_composition("A support cannot declare the same target layout more than once");
            }
        }
    }
    if (!valid_role_mask(definition.roles)) {
        invalid_composition("A support role mask must be Client, Server, or All");
    }
    if (!valid_image(definition.image)) {
        invalid_composition("A support image is required and must be a recognized TargetImage");
    }
    if (definition.failure_policy && !valid_failure_policy(*definition.failure_policy)) {
        invalid_composition("A support contains an invalid failure policy");
    }
    // Validate optional metadata only after the required target tuple fields are known to be meaningful.
    validate_relationships(definition.depends_on);
    validate_relationships(definition.includes);
    if (definition.settings) {
        validate_settings_schema(*definition.settings);
    }
}

template <class Settings> void validate_patch_definition(const PatchDefinition<Settings>& definition) {
    // Required identity, support, and failure policy facts must be complete before optional metadata is considered.
    require_id(definition.id, "A patch ID");
    if (reserved_catalog_id(definition.id)) {
        invalid_composition("FusionCutter and General are reserved patch IDs");
    }
    if (definition.name.empty()) {
        invalid_composition("A patch display name is required");
    }
    if (definition.supports.empty()) {
        invalid_composition("A patch must declare at least one support");
    }
    if (!valid_failure_policy(definition.failure_policy)) {
        invalid_composition("A patch contains an invalid failure policy");
    }
    if (definition.category) {
        require_id(*definition.category, "A patch category ID");
    }
    // Relationship and schema declarations are validated here because they become borrowed registration views later.
    validate_relationships(definition.depends_on);
    validate_relationships(definition.includes);
    if (definition.settings) {
        validate_settings_schema(*definition.settings);
    }
}

// Function traits normalize supported C++ signatures before lowering a physical native call.
template <class> inline constexpr bool dependent_false_v = false;

enum class OrdinaryCallingConvention {
    Cdecl,
    Stdcall,
    Fastcall,
    Thiscall,
    X64,
};

template <class T> struct FunctionTraits;

template <class Result, class... Args> struct FunctionTraits<Result(Args...)> {
    using result = Result;
    using arguments = std::tuple<Args...>;
    using signature = Result(Args...);
    static constexpr bool is_noexcept = false;
    static constexpr bool is_variadic = false;
};

template <class Result, class... Args>
struct FunctionTraits<Result(Args...) noexcept> : FunctionTraits<Result(Args...)> {
    static constexpr bool is_noexcept = true;
};

template <class Result, class... Args> struct FunctionTraits<Result(Args..., ...)> : FunctionTraits<Result(Args...)> {
    static constexpr bool is_variadic = true;
};

template <class Result, class... Args>
struct FunctionTraits<Result(Args..., ...) noexcept> : FunctionTraits<Result(Args..., ...)> {
    static constexpr bool is_noexcept = true;
};

template <class Result, class... Args> struct FunctionTraits<Result (*)(Args...)> : FunctionTraits<Result(Args...)> {
#if defined(_M_IX86)
    static constexpr OrdinaryCallingConvention calling_convention = OrdinaryCallingConvention::Cdecl;
#else
    static constexpr OrdinaryCallingConvention calling_convention = OrdinaryCallingConvention::X64;
#endif
};

template <class Result, class... Args>
struct FunctionTraits<Result (*)(Args...) noexcept> : FunctionTraits<Result(Args...) noexcept> {
#if defined(_M_IX86)
    static constexpr OrdinaryCallingConvention calling_convention = OrdinaryCallingConvention::Cdecl;
#else
    static constexpr OrdinaryCallingConvention calling_convention = OrdinaryCallingConvention::X64;
#endif
};

template <class Result, class... Args>
struct FunctionTraits<Result (*)(Args..., ...)> : FunctionTraits<Result(Args..., ...)> {
#if defined(_M_IX86)
    static constexpr OrdinaryCallingConvention calling_convention = OrdinaryCallingConvention::Cdecl;
#else
    static constexpr OrdinaryCallingConvention calling_convention = OrdinaryCallingConvention::X64;
#endif
};

template <class Result, class... Args>
struct FunctionTraits<Result (*)(Args..., ...) noexcept> : FunctionTraits<Result(Args..., ...) noexcept> {
#if defined(_M_IX86)
    static constexpr OrdinaryCallingConvention calling_convention = OrdinaryCallingConvention::Cdecl;
#else
    static constexpr OrdinaryCallingConvention calling_convention = OrdinaryCallingConvention::X64;
#endif
};

#if defined(_M_IX86)
template <class Result, class... Args>
struct FunctionTraits<Result(__stdcall*)(Args...)> : FunctionTraits<Result(Args...)> {
    static constexpr OrdinaryCallingConvention calling_convention = OrdinaryCallingConvention::Stdcall;
};

template <class Result, class... Args>
struct FunctionTraits<Result(__stdcall*)(Args...) noexcept> : FunctionTraits<Result(Args...) noexcept> {
    static constexpr OrdinaryCallingConvention calling_convention = OrdinaryCallingConvention::Stdcall;
};

template <class Result, class... Args>
struct FunctionTraits<Result(__fastcall*)(Args...)> : FunctionTraits<Result(Args...)> {
    static constexpr OrdinaryCallingConvention calling_convention = OrdinaryCallingConvention::Fastcall;
};

template <class Result, class... Args>
struct FunctionTraits<Result(__fastcall*)(Args...) noexcept> : FunctionTraits<Result(Args...) noexcept> {
    static constexpr OrdinaryCallingConvention calling_convention = OrdinaryCallingConvention::Fastcall;
};

template <class Result, class... Args>
struct FunctionTraits<Result(__thiscall*)(Args...)> : FunctionTraits<Result(Args...)> {
    static constexpr OrdinaryCallingConvention calling_convention = OrdinaryCallingConvention::Thiscall;
};

template <class Result, class... Args>
struct FunctionTraits<Result(__thiscall*)(Args...) noexcept> : FunctionTraits<Result(Args...) noexcept> {
    static constexpr OrdinaryCallingConvention calling_convention = OrdinaryCallingConvention::Thiscall;
};
#endif

template <class Signature, class Layout>
struct FunctionTraits<NativeCall<Signature, Layout>> : FunctionTraits<Signature> {
    using layout = Layout;
};

template <class Call> using CallResult = typename FunctionTraits<Call>::result;

template <class Call> using CallArguments = typename FunctionTraits<Call>::arguments;

// Converts one logical C++ value type into the stable ABI description used by planning and hook validation.
template <class T> [[nodiscard]] consteval FC_NativeValue native_value() {
    using Value = std::remove_cv_t<T>;
    static_assert(!std::is_reference_v<T>, "Native call values cannot be C++ references");
    if constexpr (std::is_void_v<Value>) {
        return {.kind = FC_NATIVE_VOID, .size = 0, .alignment = 0};
    } else if constexpr (std::is_pointer_v<Value>) {
        return {.kind = FC_NATIVE_POINTER, .size = sizeof(Value), .alignment = alignof(Value)};
    } else if constexpr (std::same_as<Value, float>) {
        return {.kind = FC_NATIVE_FLOAT_32, .size = sizeof(Value), .alignment = alignof(Value)};
    } else if constexpr (std::same_as<Value, double>) {
        return {.kind = FC_NATIVE_FLOAT_64, .size = sizeof(Value), .alignment = alignof(Value)};
    } else if constexpr (std::integral<Value> || std::is_enum_v<Value>) {
        static_assert(sizeof(Value) == 1 || sizeof(Value) == 2 || sizeof(Value) == 4 || sizeof(Value) == 8,
                      "Native integer values must occupy 1, 2, 4, or 8 bytes");
        return {.kind = FC_NATIVE_INTEGER, .size = sizeof(Value), .alignment = alignof(Value)};
    } else if constexpr (std::floating_point<Value>) {
        static_assert(dependent_false_v<Value>, "Native calls support only float and double floating values");
    } else {
        static_assert(std::is_standard_layout_v<Value> && std::is_trivially_copyable_v<Value>,
                      "A native record must be standard-layout and trivially copyable");
        static_assert(sizeof(Value) != 0 && std::has_single_bit(alignof(Value)) && alignof(Value) <= sizeof(Value) &&
                          sizeof(Value) % alignof(Value) == 0,
                      "A native record must have a valid complete size and alignment");
        return {.kind = FC_NATIVE_RECORD, .size = sizeof(Value), .alignment = alignof(Value)};
    }
}

template <class Home> struct HomeStorage;

template <std::size_t Offset> struct HomeStorage<abi::stack<Offset>> {
    static constexpr FC_NativeStorage value{.kind = FC_NATIVE_STORAGE_STACK,
                                            .register_id = FC_REGISTER_NONE,
                                            .stack_offset = static_cast<std::uint32_t>(Offset)};
};

#define FC_SDK_HOME_STORAGE(type, value_name)                                                                          \
    template <> struct HomeStorage<abi::type> {                                                                        \
        static constexpr FC_NativeStorage value{                                                                       \
            .kind = FC_NATIVE_STORAGE_REGISTER, .register_id = value_name, .stack_offset = 0};                         \
    }
FC_SDK_HOME_STORAGE(eax, FC_REGISTER_EAX);
FC_SDK_HOME_STORAGE(ebx, FC_REGISTER_EBX);
FC_SDK_HOME_STORAGE(ecx, FC_REGISTER_ECX);
FC_SDK_HOME_STORAGE(edx, FC_REGISTER_EDX);
FC_SDK_HOME_STORAGE(esi, FC_REGISTER_ESI);
FC_SDK_HOME_STORAGE(edi, FC_REGISTER_EDI);
FC_SDK_HOME_STORAGE(ebp, FC_REGISTER_EBP);
FC_SDK_HOME_STORAGE(st0, FC_REGISTER_ST0);
FC_SDK_HOME_STORAGE(rax, FC_REGISTER_RAX);
FC_SDK_HOME_STORAGE(rbx, FC_REGISTER_RBX);
FC_SDK_HOME_STORAGE(rcx, FC_REGISTER_RCX);
FC_SDK_HOME_STORAGE(rdx, FC_REGISTER_RDX);
FC_SDK_HOME_STORAGE(rsi, FC_REGISTER_RSI);
FC_SDK_HOME_STORAGE(rdi, FC_REGISTER_RDI);
FC_SDK_HOME_STORAGE(rbp, FC_REGISTER_RBP);
FC_SDK_HOME_STORAGE(r8, FC_REGISTER_R8);
FC_SDK_HOME_STORAGE(r9, FC_REGISTER_R9);
FC_SDK_HOME_STORAGE(r10, FC_REGISTER_R10);
FC_SDK_HOME_STORAGE(r11, FC_REGISTER_R11);
FC_SDK_HOME_STORAGE(r12, FC_REGISTER_R12);
FC_SDK_HOME_STORAGE(r13, FC_REGISTER_R13);
FC_SDK_HOME_STORAGE(r14, FC_REGISTER_R14);
FC_SDK_HOME_STORAGE(r15, FC_REGISTER_R15);
FC_SDK_HOME_STORAGE(xmm0, FC_REGISTER_XMM0);
FC_SDK_HOME_STORAGE(xmm1, FC_REGISTER_XMM1);
FC_SDK_HOME_STORAGE(xmm2, FC_REGISTER_XMM2);
FC_SDK_HOME_STORAGE(xmm3, FC_REGISTER_XMM3);
FC_SDK_HOME_STORAGE(xmm4, FC_REGISTER_XMM4);
FC_SDK_HOME_STORAGE(xmm5, FC_REGISTER_XMM5);
FC_SDK_HOME_STORAGE(xmm6, FC_REGISTER_XMM6);
FC_SDK_HOME_STORAGE(xmm7, FC_REGISTER_XMM7);
FC_SDK_HOME_STORAGE(xmm8, FC_REGISTER_XMM8);
FC_SDK_HOME_STORAGE(xmm9, FC_REGISTER_XMM9);
FC_SDK_HOME_STORAGE(xmm10, FC_REGISTER_XMM10);
FC_SDK_HOME_STORAGE(xmm11, FC_REGISTER_XMM11);
FC_SDK_HOME_STORAGE(xmm12, FC_REGISTER_XMM12);
FC_SDK_HOME_STORAGE(xmm13, FC_REGISTER_XMM13);
FC_SDK_HOME_STORAGE(xmm14, FC_REGISTER_XMM14);
FC_SDK_HOME_STORAGE(xmm15, FC_REGISTER_XMM15);
#undef FC_SDK_HOME_STORAGE

template <class Cleanup> [[nodiscard]] consteval FC_StackCleanup stack_cleanup() {
    if constexpr (std::same_as<Cleanup, abi::caller_cleanup>) {
        return FC_STACK_CLEANUP_CALLER;
    } else if constexpr (std::same_as<Cleanup, abi::callee_cleanup>) {
        return FC_STACK_CLEANUP_CALLEE;
    } else {
        return FC_STACK_CLEANUP_NONE;
    }
}

// Owns argument storage so FC_NativeCall views remain stable through the receiving Plan callback submission.
struct NativeCallStorage {
    std::vector<FC_NativeArgument> arguments;
    FC_NativeCall call{};

    void finish(FC_NativeValue result, FC_NativeStorage return_storage, FC_StackCleanup cleanup,
                std::uint32_t stack_size) noexcept {
        call = {.struct_size = sizeof(FC_NativeCall),
                .result = result,
                .return_storage = return_storage,
                .arguments = arguments.data(),
                .argument_count = static_cast<std::uint32_t>(arguments.size()),
                .cleanup = cleanup,
                .stack_size = stack_size};
    }
};

[[nodiscard]] constexpr std::uint32_t align_up(std::uint32_t value, std::uint32_t alignment) noexcept {
    return (value + alignment - 1) & ~(alignment - 1);
}

// Maps a result using the compiler's native ABI to its one supported physical return home on the current architecture.
template <class Result> [[nodiscard]] consteval FC_NativeStorage ordinary_return_storage() {
    if constexpr (std::is_void_v<Result>) {
        return {};
#if defined(_M_IX86)
    } else if constexpr (std::floating_point<Result>) {
        return HomeStorage<abi::st0>::value;
    } else if constexpr (std::integral<Result> || std::is_enum_v<Result> || std::is_pointer_v<Result>) {
        static_assert(
            sizeof(Result) <= sizeof(std::uint32_t),
            "An x86 result split across EDX:EAX is unsupported; use an output pointer or reviewed explicit layout");
        return HomeStorage<abi::eax>::value;
    } else {
        static_assert(
            dependent_false_v<Result>,
            "Record returns using the compiler's native ABI require an explicit fc::NativeCall hidden result layout");
#else
    } else if constexpr (std::floating_point<Result>) {
        return HomeStorage<abi::xmm0>::value;
    } else if constexpr (std::integral<Result> || std::is_enum_v<Result> || std::is_pointer_v<Result>) {
        static_assert(sizeof(Result) <= sizeof(std::uint64_t),
                      "A Windows x64 scalar result cannot exceed one register");
        return HomeStorage<abi::rax>::value;
    } else {
        static_assert(
            dependent_false_v<Result>,
            "Record returns using the compiler's native ABI require an explicit fc::NativeCall hidden result layout");
#endif
    }
}

template <class T>
inline constexpr bool fastcall_register_value_v =
    (std::integral<std::remove_cv_t<T>> || std::is_enum_v<std::remove_cv_t<T>> || std::is_pointer_v<T>) &&
    sizeof(T) <= sizeof(std::uint32_t);

template <OrdinaryCallingConvention Convention, class Tuple, std::size_t... Indexes>
void fill_ordinary_arguments(NativeCallStorage& storage, std::index_sequence<Indexes...>) {
#if defined(_M_IX86)
    // x86 homes depend on the declared calling convention; remaining values occupy aligned stack slots.
    std::uint32_t offset = 0;
    std::uint32_t register_count = 0;
    const auto append = [&]<std::size_t Index>() {
        using Argument = std::tuple_element_t<Index, Tuple>;
        const auto value = native_value<Argument>();
        FC_NativeStorage home{};
        if constexpr (Convention == OrdinaryCallingConvention::Thiscall && Index == 0) {
            static_assert(fastcall_register_value_v<Argument>,
                          "The first logical __thiscall argument must fit the x86 ECX register");
            home = {.kind = FC_NATIVE_STORAGE_REGISTER, .register_id = FC_REGISTER_ECX, .stack_offset = 0};
        } else if constexpr (Convention == OrdinaryCallingConvention::Fastcall && fastcall_register_value_v<Argument>) {
            if (register_count < 2) {
                home = {.kind = FC_NATIVE_STORAGE_REGISTER,
                        .register_id = register_count == 0 ? FC_REGISTER_ECX : FC_REGISTER_EDX,
                        .stack_offset = 0};
                ++register_count;
            }
        }
        if (home.kind == FC_NATIVE_STORAGE_NONE) {
            offset = align_up(offset, 4);
            home = {.kind = FC_NATIVE_STORAGE_STACK, .register_id = FC_REGISTER_NONE, .stack_offset = offset};
            offset += align_up(std::max<std::uint32_t>(value.size, 4), 4);
        }
        storage.arguments.push_back({.value = value, .storage = home});
    };
    (append.template operator()<Indexes>(), ...);
#else
    // Windows x64 assigns the first four logical positions to paired integer/SIMD homes, then uses the stack.
    static_assert(Convention == OrdinaryCallingConvention::X64);
    constexpr std::array integer_registers{FC_REGISTER_RCX, FC_REGISTER_RDX, FC_REGISTER_R8, FC_REGISTER_R9};
    constexpr std::array float_registers{FC_REGISTER_XMM0, FC_REGISTER_XMM1, FC_REGISTER_XMM2, FC_REGISTER_XMM3};
    const auto append = [&]<std::size_t Index>() {
        using Argument = std::tuple_element_t<Index, Tuple>;
        const auto value = native_value<Argument>();
        static_assert(native_value<Argument>().kind != FC_NATIVE_RECORD || sizeof(Argument) <= 8,
                      "A record argument using the compiler's native Windows x64 ABI must fit one eight-byte home");
        FC_NativeStorage home{};
        if constexpr (Index < 4) {
            home = {.kind = FC_NATIVE_STORAGE_REGISTER,
                    .register_id = std::floating_point<std::remove_cv_t<Argument>> ? float_registers[Index]
                                                                                   : integer_registers[Index],
                    .stack_offset = 0};
        } else {
            home = {.kind = FC_NATIVE_STORAGE_STACK,
                    .register_id = FC_REGISTER_NONE,
                    .stack_offset = static_cast<std::uint32_t>((Index - 4) * 8)};
        }
        storage.arguments.push_back({.value = value, .storage = home});
    };
    (append.template operator()<Indexes>(), ...);
#endif
}

template <class Tuple, class... Homes, std::size_t... Indexes>
void fill_explicit_arguments(NativeCallStorage& storage, abi::args<Homes...>, std::index_sequence<Indexes...>) {
    static_assert(sizeof...(Homes) == sizeof...(Indexes),
                  "fc::abi::args must provide exactly one home for each logical argument");
    using HomeTuple = std::tuple<Homes...>;
    (storage.arguments.push_back({.value = native_value<std::tuple_element_t<Indexes, Tuple>>(),
                                  .storage = HomeStorage<std::tuple_element_t<Indexes, HomeTuple>>::value}),
     ...);
}

template <class Result, class Home>
[[nodiscard]] consteval FC_NativeStorage explicit_return_storage(abi::result<Home>) {
    if constexpr (std::is_void_v<Result>) {
        static_assert(std::is_void_v<Home>, "A void native result uses fc::abi::result<void>");
        return {};
    } else {
        static_assert(!std::is_void_v<Home>, "A non-void native result requires one physical home");
        return HomeStorage<Home>::value;
    }
}

template <class Result, class Home>
[[nodiscard]] consteval FC_NativeStorage explicit_return_storage(abi::hidden_result<Home>) {
    static_assert(!std::is_void_v<Home>, "A hidden native result requires one pointer home");
    return HomeStorage<Home>::value;
}

template <class Return> struct ReturnLayoutTraits;

template <class Home> struct ReturnLayoutTraits<abi::result<Home>> {
    using home = Home;
    static constexpr bool hidden = false;
};

template <class Home> struct ReturnLayoutTraits<abi::hidden_result<Home>> {
    using home = Home;
    static constexpr bool hidden = true;
};

template <class Layout> struct LayoutTraits;

template <class Arguments, class Return, class Cleanup> struct LayoutTraits<abi::x86<Arguments, Return, Cleanup>> {
    using arguments = Arguments;
    using result = Return;
    using cleanup = Cleanup;
    static constexpr Architecture architecture = Architecture::X86;
};

template <class Arguments, class Return, class Cleanup> struct LayoutTraits<abi::x64<Arguments, Return, Cleanup>> {
    using arguments = Arguments;
    using result = Return;
    using cleanup = Cleanup;
    static constexpr Architecture architecture = Architecture::X64;
};

// Explicit layouts are accepted only when every logical value fits a legal, nonoverlapping physical home.
template <Architecture TargetArchitecture, class Value, class Home> [[nodiscard]] consteval bool valid_home() {
    constexpr auto native = native_value<Value>();
    constexpr auto storage = HomeStorage<Home>::value;
    if constexpr (requires { Home::offset; }) {
        return native.kind != FC_NATIVE_VOID && native.alignment != 0 && Home::offset % native.alignment == 0;
    } else {
        constexpr auto register_id = storage.register_id;
        constexpr bool x86_general = register_id >= FC_REGISTER_EAX && register_id <= FC_REGISTER_EBP;
        constexpr bool x64_general = register_id >= FC_REGISTER_RAX && register_id <= FC_REGISTER_R15;
        constexpr bool x86_simd = register_id >= FC_REGISTER_XMM0 && register_id <= FC_REGISTER_XMM7;
        constexpr bool x64_simd = register_id >= FC_REGISTER_XMM0 && register_id <= FC_REGISTER_XMM15;
        constexpr bool general = TargetArchitecture == Architecture::X86 ? x86_general : x64_general;
        constexpr bool simd = TargetArchitecture == Architecture::X86 ? x86_simd : x64_simd;
        constexpr std::uint32_t general_size = TargetArchitecture == Architecture::X86 ? 4 : 8;
        if constexpr (std::same_as<Home, abi::st0>) {
            return TargetArchitecture == Architecture::X86 &&
                   (native.kind == FC_NATIVE_FLOAT_32 || native.kind == FC_NATIVE_FLOAT_64);
        }
        if (native.kind == FC_NATIVE_FLOAT_32 || native.kind == FC_NATIVE_FLOAT_64) {
            return simd;
        }
        return general && native.kind != FC_NATIVE_VOID && native.size <= general_size;
    }
}

template <Architecture TargetArchitecture, class Tuple, class... Homes, std::size_t... Indexes>
[[nodiscard]] consteval bool valid_explicit_argument_homes(abi::args<Homes...>, std::index_sequence<Indexes...>) {
    if constexpr (sizeof...(Homes) != sizeof...(Indexes)) {
        return false;
    } else {
        // Materialize parallel constexpr arrays so pairwise register and stack overlap checks share one representation.
        constexpr std::array<FC_NativeValue, sizeof...(Homes)> values{
            native_value<std::tuple_element_t<Indexes, Tuple>>()...};
        constexpr std::array<FC_NativeStorage, sizeof...(Homes)> storage{HomeStorage<Homes>::value...};
        if (!(valid_home<TargetArchitecture, std::tuple_element_t<Indexes, Tuple>, Homes>() && ...)) {
            return false;
        }
        // No two logical arguments may claim the same register or overlapping stack byte extent.
        for (std::size_t left = 0; left < storage.size(); ++left) {
            for (std::size_t right = left + 1; right < storage.size(); ++right) {
                if (storage[left].kind != storage[right].kind) {
                    continue;
                }
                if (storage[left].kind == FC_NATIVE_STORAGE_REGISTER &&
                    storage[left].register_id == storage[right].register_id) {
                    return false;
                }
                if (storage[left].kind == FC_NATIVE_STORAGE_STACK) {
                    const auto left_end = storage[left].stack_offset + values[left].size;
                    const auto right_end = storage[right].stack_offset + values[right].size;
                    if (storage[left].stack_offset < right_end && storage[right].stack_offset < left_end) {
                        return false;
                    }
                }
            }
        }
        return true;
    }
}

template <class Home> [[nodiscard]] consteval std::uint32_t home_end(FC_NativeValue value) {
    if constexpr (requires { Home::offset; }) {
        return Home::offset + value.size;
    } else {
        return std::uint8_t{0};
    }
}

template <class Call> [[nodiscard]] NativeCallStorage native_call_storage() {
    using Traits = FunctionTraits<Call>;
    using ResultType = typename Traits::result;
    using Arguments = typename Traits::arguments;
    NativeCallStorage storage;
    storage.arguments.reserve(std::tuple_size_v<Arguments>);

    if constexpr (is_explicit_native_call_v<Call>) {
        // Reviewed layouts state every physical home and are checked entirely before materializing the ABI table.
        using Layout = typename Traits::layout;
        using Physical = LayoutTraits<Layout>;
        using ReturnLayout = ReturnLayoutTraits<typename Physical::result>;
        using ReturnHome = typename ReturnLayout::home;
#if defined(_M_IX86)
        static_assert(Physical::architecture == Architecture::X86,
                      "An x86 plugin cannot use an fc::abi::x64 native call layout");
#else
        static_assert(Physical::architecture == Architecture::X64,
                      "An x64 plugin cannot use an fc::abi::x86 native call layout");
#endif
        static_assert(!Traits::is_variadic, "An explicit fc::NativeCall signature cannot be variadic");
        static_assert(valid_explicit_argument_homes<Physical::architecture, Arguments>(
                          typename Physical::arguments{}, std::make_index_sequence<std::tuple_size_v<Arguments>>{}),
                      "Explicit native call argument homes must be valid, complete, and nonoverlapping");
        constexpr auto result_value = native_value<ResultType>();
        if constexpr (ReturnLayout::hidden) {
            static_assert(result_value.kind == FC_NATIVE_RECORD,
                          "fc::abi::hidden_result is reserved for a native record result");
            static_assert(valid_home<Physical::architecture, void*, ReturnHome>(),
                          "A hidden native result requires one pointer-compatible physical home");
        } else if constexpr (std::is_void_v<ResultType>) {
            static_assert(std::is_void_v<ReturnHome>, "A void native result uses fc::abi::result<void>");
        } else {
            static_assert(result_value.kind != FC_NATIVE_RECORD,
                          "A native record result requires fc::abi::hidden_result");
            static_assert(valid_home<Physical::architecture, ResultType, ReturnHome>(),
                          "The explicit native result home cannot contain its logical result type");
        }
        fill_explicit_arguments<Arguments>(storage, typename Physical::arguments{},
                                           std::make_index_sequence<std::tuple_size_v<Arguments>>{});
        // The stack extent includes ordinary arguments and the hidden result pointer's stack home.
        std::uint32_t stack_size = 0;
        for (const auto& argument : storage.arguments) {
            if (argument.storage.kind == FC_NATIVE_STORAGE_STACK) {
                stack_size = std::max(stack_size, argument.storage.stack_offset + argument.value.size);
            }
        }
        if constexpr (ReturnLayout::hidden && requires { ReturnHome::offset; }) {
            stack_size = std::max(stack_size, static_cast<std::uint32_t>(ReturnHome::offset + sizeof(void*)));
        }
        if constexpr (Physical::architecture == Architecture::X86) {
            static_assert(std::same_as<typename Physical::cleanup, abi::caller_cleanup> ||
                              std::same_as<typename Physical::cleanup, abi::callee_cleanup> ||
                              std::same_as<typename Physical::cleanup, abi::no_cleanup>,
                          "An x86 native call requires a recognized cleanup policy");
            if (stack_size != 0 && std::same_as<typename Physical::cleanup, abi::no_cleanup>) {
                throw std::invalid_argument{"An x86 native call with stack homes requires caller or callee cleanup"};
            }
            stack_size = align_up(stack_size, 4);
        } else {
            static_assert(std::same_as<typename Physical::cleanup, abi::no_cleanup>,
                          "A Windows x64 native call requires fc::abi::no_cleanup");
            stack_size = align_up(stack_size, 8);
        }
        storage.finish(native_value<ResultType>(), explicit_return_storage<ResultType>(typename Physical::result{}),
                       stack_cleanup<typename Physical::cleanup>(), stack_size);
    } else if constexpr (!Traits::is_variadic) {
        // Ordinary function pointers use the compiler ABI inferred from their type and the current build architecture.
        fill_ordinary_arguments<Traits::calling_convention, Arguments>(
            storage, std::make_index_sequence<std::tuple_size_v<Arguments>>{});
        std::uint32_t stack_size = 0;
        for (const auto& argument : storage.arguments) {
            if (argument.storage.kind == FC_NATIVE_STORAGE_STACK) {
                stack_size = std::max(stack_size, argument.storage.stack_offset + argument.value.size);
            }
        }
#if defined(_M_IX86)
        constexpr auto cleanup = Traits::calling_convention == OrdinaryCallingConvention::Cdecl
                                     ? FC_STACK_CLEANUP_CALLER
                                     : FC_STACK_CLEANUP_CALLEE;
        storage.finish(native_value<ResultType>(), ordinary_return_storage<ResultType>(), cleanup,
                       align_up(stack_size, 4));
#else
        storage.finish(native_value<ResultType>(), ordinary_return_storage<ResultType>(), FC_STACK_CLEANUP_NONE,
                       align_up(stack_size, 8));
#endif
    }
    return storage;
}

[[nodiscard]] inline TargetInfo target_info(const FC_TargetInfo& target) noexcept {
    return {.layout = static_cast<TargetLayout>(target.layout),
            .role = static_cast<HostRole>(target.role),
            .architecture = static_cast<Architecture>(target.architecture),
            .image_profile = string_view(target.image_profile)};
}

inline CreateContext ContextFactory::create(const FC_HostApi* host, const FC_CreateContext& context) noexcept {
    return CreateContext{target_info(context.target), Logger{host, context.report}};
}

// Context adapters translate callback-scoped C tables into the narrower author-facing SDK capabilities.
struct InterfaceQueryAdapter {
    template <class Handler>
    static bool query(Handler& handler, FC_StringView id, std::uint32_t size, void* output) noexcept {
        InterfaceQuery query;
        query.requested_id_ = string_view(id);
        query.output_ = {static_cast<std::byte*>(output), size};
        handler.query_interface(query);
        return query.provided_;
    }
};

} // namespace detail

inline PrepareContext::PrepareContext(const FC_HostApi* host, const FC_PrepareContext* context) noexcept
    : host_(host), context_(context), logger_(detail::ContextFactory::logger(host, context->report)) {}

template <NativeData T> std::span<T> PrepareContext::resolve(DataHandle<T> handle) noexcept {
    // Capability failure is sticky so a handler cannot ignore an empty result and report Prepare callback success.
    if (failed_ || context_ == nullptr || context_->resolve_data == nullptr ||
        handle.handle_ == FC_INVALID_DATA_HANDLE) {
        failed_ = true;
        return {};
    }
    std::uintptr_t address = 0;
    std::uint64_t byte_size = 0;
    // The resolved allocation must match the retained type extent and alignment before exposing a typed span.
    if (context_->resolve_data(context_->context, handle.handle_, &address, &byte_size) != FC_TRUE ||
        byte_size != handle.count_ * sizeof(T) || address % alignof(T) != 0) {
        failed_ = true;
        return {};
    }
    return {reinterpret_cast<T*>(address), handle.count_};
}

template <InterfaceContract Interface>
std::optional<Interface> PrepareContext::find_interface(std::string_view provider_patch) const noexcept {
    if (context_ == nullptr || context_->find_interface == nullptr) {
        return std::nullopt;
    }
    Interface result{};
    if (context_->find_interface(context_->context, detail::string_view(provider_patch),
                                 detail::string_view(std::string_view{Interface::id}), sizeof(Interface),
                                 &result) != FC_TRUE) {
        return std::nullopt;
    }
    return result;
}

inline std::expected<TraceChannel, Error> PrepareContext::create_trace(TraceDefinition definition) {
    if (context_ == nullptr || context_->create_trace == nullptr) {
        return std::unexpected(Error{.message = "Trace creation is unavailable", .operation = "Create trace"});
    }
    const FC_TraceDefinition native{.struct_size = sizeof(FC_TraceDefinition),
                                    .name = detail::string_view(definition.name),
                                    .capacity = definition.capacity,
                                    .max_record_size = definition.max_record_size,
                                    .version = definition.version};
    FC_TraceHandle handle = nullptr;
    const auto result = context_->create_trace(context_->context, &native, &handle);
    // Disabled is an intentional inert success; rejected is the only result that fails the enclosing author operation.
    if (result == FC_TRACE_CREATED) {
        return detail::ContextFactory::trace(host_, handle);
    }
    if (result == FC_TRACE_DISABLED) {
        return TraceChannel{};
    }
    return std::unexpected(Error{.message = "The trace channel was rejected", .operation = "Create trace"});
}

} // namespace fc

namespace fc {

namespace detail {

// Type-erased lowering separates support metadata from the handler and settings types retained behind it.
struct SupportMetadata {
    std::vector<TargetLayout> layouts;
    HostRole roles{};
    TargetImage image{};
    std::vector<std::string> depends_on;
    std::vector<std::string> includes;
    std::optional<FailurePolicy> failure_policy;
};

// Owns the nested vectors behind one flat FC_SettingDefinition array until registration state is released.
struct NativeSchemaStorage {
    std::vector<FC_SettingDefinition> definitions;
    std::vector<std::vector<FC_StringView>> choices;

    template <class Settings> void append(std::string_view section, const SettingEntry<Settings>& entry) {
        const auto& data = SettingEntryAccess<Settings>::data(entry);
        if (data == nullptr) {
            throw std::invalid_argument{"A settings schema contains an empty entry"};
        }
        // Choice views require a stable nested vector even for non-choice entries, keeping definitions index-aligned.
        choices.emplace_back();
        auto& choice_views = choices.back();
        choice_views.reserve(data->choice_names.size());
        for (const auto& choice_name : data->choice_names) {
            choice_views.push_back(detail::string_view(choice_name));
        }
        auto default_value = data->default_value;
        if (data->type == FC_SETTING_STRING) {
            default_value.string_value = detail::string_view(data->default_string);
        }
        definitions.push_back({.section = detail::string_view(section),
                               .key = detail::string_view(data->key),
                               .description = detail::string_view(data->description),
                               .environment = detail::string_view(data->environment),
                               .type = data->type,
                               .has_range = data->has_range ? FC_TRUE : FC_FALSE,
                               .default_value = default_value,
                               .minimum = data->minimum,
                               .maximum = data->maximum,
                               .max_length = data->max_length,
                               .choices = choice_views.data(),
                               .choice_count = static_cast<std::uint32_t>(choice_views.size())});
    }

    template <class Settings> void append(const SettingsSchema<Settings>& schema) {
        // Reserve for the flattened schema before appending so later native views are not invalidated by growth.
        std::size_t entry_count = schema.entries.size();
        for (const auto& section : schema.sections) {
            entry_count += section.entries.size();
        }
        definitions.reserve(definitions.size() + entry_count);
        choices.reserve(choices.size() + entry_count);
        // Root entries precede named sections, matching the schema's author-visible declaration hierarchy.
        for (const auto& entry : schema.entries) {
            append(std::string_view{}, entry);
        }
        for (const auto& section : schema.sections) {
            for (const auto& entry : section.entries) {
                append(section.id, entry);
            }
        }
    }
};

// Runtime adapters own callback code and translate every C ABI lifecycle entry to an optional handler method.
class HandlerAdapterBase {
  public:
    virtual ~HandlerAdapterBase() = default;
    [[nodiscard]] virtual FC_PatchCallbacks callbacks() noexcept = 0;
};

// Capability concepts distinguish an absent optional method from a method declared with an invalid signature.
template <class Handler>
concept HasPlan = requires(Handler& handler, Plan& plan) {
    { handler.plan(plan) } -> std::same_as<void>;
};

template <class Handler>
concept HasPrepare = requires(Handler& handler, PrepareContext& context) {
    { handler.prepare(context) } -> std::same_as<Result>;
};

template <class Handler>
concept HasActivate = requires(Handler& handler, ActivateContext& context) {
    { handler.activate(context) } noexcept -> std::same_as<void>;
};

template <class Handler>
concept HasUpdate = requires(Handler& handler, UpdateContext& context) {
    { handler.update(context) } noexcept -> std::same_as<void>;
};

template <class Handler>
concept HasStatus = requires(const Handler& handler, StatusWriter& writer) {
    { handler.write_status(writer) } noexcept -> std::same_as<void>;
};

template <class Handler>
concept HasInterfaceQuery = requires(Handler& handler, InterfaceQuery& query) {
    { handler.query_interface(query) } noexcept -> std::same_as<void>;
};

template <class Handler>
concept NamesPlan = requires { &Handler::plan; };

template <class Handler>
concept NamesPrepare = requires { &Handler::prepare; };

template <class Handler>
concept NamesActivate = requires { &Handler::activate; };

template <class Handler>
concept NamesUpdate = requires { &Handler::update; };

template <class Handler>
concept NamesStatus = requires { &Handler::write_status; };

template <class Handler>
concept NamesInterfaceQuery = requires { &Handler::query_interface; };

// Retains type metadata to begin one symbolic allocation's object lifetime during the Prepare phase.
struct AllocationBinding {
    FC_DataHandle handle = FC_INVALID_DATA_HANDLE;
    std::size_t count{};
    std::size_t element_size{};
    std::size_t alignment{};
    void (*start_lifetime)(void* address, std::size_t count) noexcept = nullptr;
};

// Per-instance bindings keep callback objects and planned allocation metadata alive across later phases.
struct PatchRuntimeBindings {
    std::vector<std::shared_ptr<void>> callbacks;
    std::vector<AllocationBinding> allocations;
};

template <class Handler>
FC_CallStatus invoke_plan(Handler& handler, const FC_HostApi* host, const FC_PlanContext* plan_context,
                          const FC_PlanSink* sink, PatchRuntimeBindings& bindings, const FC_ErrorSink* error);

template <class Settings>
[[nodiscard]] std::unique_ptr<Settings> construct_settings(const SettingsSchema<Settings>& schema,
                                                           const FC_SettingsView* values, const FC_ErrorSink* error) {
    if (values == nullptr) {
        throw std::invalid_argument{"Typed settings were not supplied"};
    }
    // The flat native view must match the schema's deterministic order: top-level entries followed by each section.
    std::size_t expected_count = schema.entries.size();
    for (const auto& section : schema.sections) {
        expected_count += section.entries.size();
    }
    if (values->count != expected_count || (expected_count != 0 && values->values == nullptr)) {
        throw std::invalid_argument{"The resolved settings count does not match the selected schema"};
    }
    // Assignment reconstructs the typed object in the same order used while lowering the schema.
    auto result = std::make_unique<Settings>();
    std::size_t index = 0;
    const auto assign = [&](const SettingEntry<Settings>& entry) {
        const auto& data = SettingEntryAccess<Settings>::data(entry);
        data->assign(*result, values->values[index]);
        ++index;
    };
    for (const auto& entry : schema.entries) {
        assign(entry);
    }
    for (const auto& section : schema.sections) {
        for (const auto& entry : section.entries) {
            assign(entry);
        }
    }
    // Settings validation sees the complete object and may normalize it before handler construction.
    if (schema.validate != nullptr) {
        auto validation = schema.validate(*result);
        if (!validation) {
            set_error(error, validation.error().message, validation.error().operation);
            throw std::invalid_argument{"The typed settings validator rejected the completed value"};
        }
    }
    return result;
}

// Resolves every planned allocation and begins object lifetime before the author Prepare callback can access it.
inline bool prepare_allocations(PatchRuntimeBindings& bindings, const FC_PrepareContext& context,
                                const FC_ErrorSink* error) noexcept {
    if (bindings.allocations.empty()) {
        return true;
    }
    if (context.resolve_data == nullptr) {
        set_error(error, "Native data resolution is unavailable", "Prepare native allocations");
        return false;
    }
    for (const auto& binding : bindings.allocations) {
        std::uintptr_t address = 0;
        std::uint64_t byte_size = 0;
        if (context.resolve_data(context.context, binding.handle, &address, &byte_size) != FC_TRUE ||
            byte_size != binding.count * binding.element_size || address % binding.alignment != 0) {
            set_error(error, "A planned native allocation could not be resolved", "Prepare native allocations");
            return false;
        }
        binding.start_lifetime(reinterpret_cast<void*>(address), binding.count);
    }
    return true;
}

// One adapter binds a handler type and effective schema to the stable C callback table retained by registration.
template <class Handler> class HandlerAdapter final : public HandlerAdapterBase {
  public:
    using Settings = SettingsFor<Handler>;

    HandlerAdapter(const FC_HostApi* host, std::optional<SettingsSchema<Settings>> schema,
                   std::shared_ptr<const void> seed = {})
        : host_(host), schema_(std::move(schema)), seed_(std::move(seed)) {
        // Checks for named methods focus diagnostics when an optional lifecycle function has the wrong signature.
        static_assert(std::is_nothrow_destructible_v<Handler>, "Patch handlers must be non-throwingly destructible");
        static_assert(!NamesPlan<Handler> || HasPlan<Handler>, "Handler::plan must have signature void(fc::Plan&)");
        static_assert(!NamesPrepare<Handler> || HasPrepare<Handler>,
                      "Handler::prepare must have signature fc::Result(fc::PrepareContext&)");
        static_assert(!NamesActivate<Handler> || HasActivate<Handler>,
                      "Handler::activate must have signature void(fc::ActivateContext&) noexcept");
        static_assert(!NamesUpdate<Handler> || HasUpdate<Handler>,
                      "Handler::update must have signature void(fc::UpdateContext&) noexcept");
        static_assert(!NamesStatus<Handler> || HasStatus<Handler>,
                      "Handler::write_status must have signature void(fc::StatusWriter&) const noexcept");
        static_assert(!NamesInterfaceQuery<Handler> || HasInterfaceQuery<Handler>,
                      "Handler::query_interface must have signature void(fc::InterfaceQuery&) noexcept");
        // Construction rules differ only at this boundary; later callbacks operate on one uniform Instance owner.
        if constexpr (std::same_as<Settings, NoSettings>) {
            static_assert(std::constructible_from<Handler, const CreateContext&> ||
                              std::default_initializable<Handler> ||
                              std::constructible_from<Handler, const CreateContext&, std::shared_ptr<const void>>,
                          "A handler without settings needs Handler(const CreateContext&) or Handler()");
        } else {
            static_assert(std::default_initializable<Settings>, "Handler::Settings must be default-initializable");
            static_assert(std::is_nothrow_destructible_v<Settings>,
                          "Handler::Settings must be non-throwingly destructible");
            static_assert(std::constructible_from<Handler, const CreateContext&, const Settings&> ||
                              std::constructible_from<Handler, const CreateContext&, std::unique_ptr<Settings>,
                                                      std::shared_ptr<const void>>,
                          "A typed handler needs Handler(const CreateContext&, const Settings&)");
            if (!schema_) {
                throw std::invalid_argument{"A typed handler has no effective settings schema"};
            }
        }
    }

    [[nodiscard]] FC_PatchCallbacks callbacks() noexcept override {
        // Unsupported optional capabilities remain null, allowing the framework to detect them without invocation.
        return {.context = this,
                .create = &create,
                .destroy = &destroy,
                .plan = &plan,
                .prepare = &prepare,
                .activate = &activate,
                .update = HasUpdate<Handler> ? &update : nullptr,
                .write_status = HasStatus<Handler> ? &write_status : nullptr,
                .query_interface = HasInterfaceQuery<Handler> ? &query_interface : nullptr};
    }

  private:
    // One native patch handle owns the author handler and callbacks or allocations retained by its accepted patch plan.
    struct Instance {
        template <class... Args> explicit Instance(Args&&... args) : handler(std::forward<Args>(args)...) {}
        Handler handler;
        PatchRuntimeBindings bindings;
    };

    // Constructs the typed handler and settings behind one opaque patch handle without leaking author exceptions.
    static FC_CallStatus FC_CALL create(void* context, const FC_CreateContext* create_context,
                                        const FC_SettingsView* settings, const FC_ErrorSink* error,
                                        FC_PatchHandle* output) {
        if (output == nullptr || create_context == nullptr) {
            return FC_CALL_FAILED;
        }
        *output = nullptr;
        auto& adapter = *static_cast<HandlerAdapter*>(context);
        try {
            // Settings are reconstructed before construction, and no author exception may cross this C entry point.
            auto author_context = ContextFactory::create(adapter.host_, *create_context);
            if constexpr (std::same_as<Settings, NoSettings>) {
                if constexpr (std::constructible_from<Handler, const CreateContext&, std::shared_ptr<const void>>) {
                    *output = new Instance{author_context, adapter.seed_};
                } else if constexpr (std::constructible_from<Handler, const CreateContext&>) {
                    *output = new Instance{author_context};
                } else {
                    *output = new Instance{};
                }
            } else {
                auto typed_settings = construct_settings(*adapter.schema_, settings, error);
                if constexpr (std::constructible_from<Handler, const CreateContext&, std::unique_ptr<Settings>,
                                                      std::shared_ptr<const void>>) {
                    *output = new Instance{author_context, std::move(typed_settings), adapter.seed_};
                } else {
                    *output = new Instance{author_context, *typed_settings};
                }
            }
            return FC_CALL_OK;
        } catch (const std::exception& exception) {
            set_error(error, exception.what(), "Construct patch");
        } catch (...) {
            set_error(error, "The patch constructor threw an unknown exception", "Construct patch");
        }
        return FC_CALL_FAILED;
    }

    // Destroy is the single release point for the handler and the plan bindings retained after installation.
    static void FC_CALL destroy(void*, FC_PatchHandle patch) noexcept {
        delete static_cast<Instance*>(patch);
    }

    // Adapts the handler's Plan method to synchronous submissions through the framework-owned plan sink.
    static FC_CallStatus FC_CALL plan(void* context, FC_PatchHandle patch, const FC_PlanContext* plan_context,
                                      const FC_PlanSink* sink, const FC_ErrorSink* error) {
        if (patch == nullptr || plan_context == nullptr || sink == nullptr) {
            return FC_CALL_FAILED;
        }
        auto& adapter = *static_cast<HandlerAdapter*>(context);
        auto& instance = *static_cast<Instance*>(patch);
        return invoke_plan(instance.handler, adapter.host_, plan_context, sink, instance.bindings, error);
    }

    // Resolves typed allocations before invoking optional fallible author preparation at the ABI exception boundary.
    static FC_CallStatus FC_CALL prepare(void* context, FC_PatchHandle patch, const FC_PrepareContext* prepare_context,
                                         const FC_ErrorSink* error) {
        if (patch == nullptr || prepare_context == nullptr) {
            return FC_CALL_FAILED;
        }
        auto& adapter = *static_cast<HandlerAdapter*>(context);
        auto& instance = *static_cast<Instance*>(patch);
        // Native allocation lifetime must begin before the handler receives any matching typed handles.
        if (!prepare_allocations(instance.bindings, *prepare_context, error)) {
            return FC_CALL_FAILED;
        }
        try {
            auto author_context = ContextFactory::prepare(adapter.host_, *prepare_context);
            if constexpr (HasPrepare<Handler>) {
                auto result = instance.handler.prepare(author_context);
                if (!result) {
                    set_error(error, result.error().message, result.error().operation);
                    return FC_CALL_FAILED;
                }
            }
            if (author_context.failed_) {
                set_error(error, "A capability requested during the Prepare callback failed", "Prepare patch");
                return FC_CALL_FAILED;
            }
            return FC_CALL_OK;
        } catch (const std::exception& exception) {
            set_error(error, exception.what(), "Prepare patch");
        } catch (...) {
            set_error(error, "The patch's Prepare callback threw an unknown exception", "Prepare patch");
        }
        return FC_CALL_FAILED;
    }

    // Remaining lifecycle trampolines expose only declared optional capabilities and are non-failing by contract.
    static void FC_CALL activate(void* context, FC_PatchHandle patch,
                                 const FC_ActivateContext* activate_context) noexcept {
        if constexpr (HasActivate<Handler>) {
            if (patch != nullptr && activate_context != nullptr) {
                auto& adapter = *static_cast<HandlerAdapter*>(context);
                auto author_context = ContextFactory::activate(adapter.host_, *activate_context);
                static_cast<Instance*>(patch)->handler.activate(author_context);
            }
        }
    }

    static void FC_CALL update(void* context, FC_PatchHandle patch, const FC_UpdateContext* update_context) noexcept {
        if constexpr (HasUpdate<Handler>) {
            if (patch != nullptr && update_context != nullptr) {
                auto& adapter = *static_cast<HandlerAdapter*>(context);
                auto author_context = ContextFactory::update(adapter.host_, *update_context);
                static_cast<Instance*>(patch)->handler.update(author_context);
            }
        }
    }

    static void FC_CALL write_status(void*, FC_PatchHandle patch, const FC_StatusSink* status) noexcept {
        if constexpr (HasStatus<Handler>) {
            if (patch != nullptr && status != nullptr) {
                auto writer = ContextFactory::status(status);
                static_cast<const Instance*>(patch)->handler.write_status(writer);
            }
        }
    }

    static FC_Bool FC_CALL query_interface(void*, FC_PatchHandle patch, FC_StringView id, std::uint32_t size,
                                           void* output) noexcept {
        if constexpr (HasInterfaceQuery<Handler>) {
            if (patch != nullptr && output != nullptr &&
                InterfaceQueryAdapter::query(static_cast<Instance*>(patch)->handler, id, size, output)) {
                return FC_TRUE;
            }
        }
        return FC_FALSE;
    }

    const FC_HostApi* host_ = nullptr;
    std::optional<SettingsSchema<Settings>> schema_;
    std::shared_ptr<const void> seed_;
};

// Carries the effective callback adapter chosen after common/support handler and schema inheritance is resolved.
struct AdapterSelection {
    std::shared_ptr<HandlerAdapterBase> adapter;
};

template <class T> [[nodiscard]] const void* type_token() noexcept {
    static const unsigned char token = 0;
    return &token;
}

} // namespace detail

namespace detail {

// Support models preserve whether settings and handler behavior are inherited or replaced for one target tuple set.
struct SupportConcept {
    virtual ~SupportConcept() = default;
    [[nodiscard]] virtual const detail::SupportMetadata& metadata() const noexcept = 0;
    [[nodiscard]] virtual bool has_settings() const noexcept = 0;
    virtual void lower_settings(detail::NativeSchemaStorage& output) const = 0;
    [[nodiscard]] virtual bool inherits_handler() const noexcept = 0;
    [[nodiscard]] virtual detail::AdapterSelection select(const FC_HostApi* host, const void* common_type,
                                                          const void* common_schema) const = 0;
};

class UntypedSupportModel final : public SupportConcept {
  public:
    explicit UntypedSupportModel(SupportDefinition<> definition)
        : metadata_{.layouts = std::move(definition.layouts),
                    .roles = definition.roles,
                    .image = definition.image,
                    .depends_on = std::move(definition.depends_on),
                    .includes = std::move(definition.includes),
                    .failure_policy = definition.failure_policy},
          settings_(std::move(definition.settings)) {}

    [[nodiscard]] const SupportMetadata& metadata() const noexcept override {
        return metadata_;
    }
    [[nodiscard]] bool has_settings() const noexcept override {
        return settings_.has_value();
    }
    [[nodiscard]] bool inherits_handler() const noexcept override {
        return true;
    }
    void lower_settings(NativeSchemaStorage& output) const override {
        if (settings_) {
            output.append(*settings_);
        }
    }

    [[nodiscard]] AdapterSelection select(const FC_HostApi*, const void*, const void*) const override {
        return {};
    }

  private:
    SupportMetadata metadata_;
    std::optional<SettingsSchema<NoSettings>> settings_;
};

template <class Handler> class TypedSupportModel final : public SupportConcept {
  public:
    using Settings = SettingsFor<Handler>;

    explicit TypedSupportModel(SupportDefinition<Settings> definition)
        : metadata_{.layouts = std::move(definition.layouts),
                    .roles = definition.roles,
                    .image = definition.image,
                    .depends_on = std::move(definition.depends_on),
                    .includes = std::move(definition.includes),
                    .failure_policy = definition.failure_policy},
          settings_(std::move(definition.settings)) {}

    [[nodiscard]] const SupportMetadata& metadata() const noexcept override {
        return metadata_;
    }
    [[nodiscard]] bool has_settings() const noexcept override {
        return settings_.has_value();
    }
    [[nodiscard]] bool inherits_handler() const noexcept override {
        return false;
    }
    void lower_settings(NativeSchemaStorage& output) const override {
        if (settings_) {
            output.append(*settings_);
        }
    }

    [[nodiscard]] AdapterSelection select(const FC_HostApi* host, const void* common_type,
                                          const void* common_schema) const override {
        // A replacement schema wins; otherwise inheritance is permitted only for the identical settings type.
        std::optional<SettingsSchema<Settings>> effective = settings_;
        if (!effective && common_type == type_token<Settings>() && common_schema != nullptr) {
            effective = *static_cast<const SettingsSchema<Settings>*>(common_schema);
        }
        if constexpr (!std::same_as<Settings, NoSettings>) {
            if (!effective) {
                throw std::invalid_argument{
                    "A typed support handler requires a matching common or replacement settings schema"};
            }
        }
        return {.adapter = std::make_shared<HandlerAdapter<Handler>>(host, std::move(effective))};
    }

  private:
    SupportMetadata metadata_;
    std::optional<SettingsSchema<Settings>> settings_;
};

} // namespace detail

// Composition errors throw invalid_argument before a factory can submit this owning support definition.
inline Support support(SupportDefinition<> definition) {
    detail::validate_support_definition(definition);
    return Support{std::make_shared<detail::UntypedSupportModel>(std::move(definition))};
}

template <class Handler> Support support(SupportDefinition<detail::SettingsFor<Handler>> definition) {
    detail::validate_support_definition(definition);
    return Support{std::make_shared<detail::TypedSupportModel<Handler>>(std::move(definition))};
}

namespace detail {

// Native lowering storage owns every pointer target exposed through the transient registration tree.
struct NativeSupportStorage {
    NativeSchemaStorage settings;
    std::vector<FC_StringView> depends_on;
    std::vector<FC_StringView> includes;
    FC_SupportDefinition native{};
};

struct NativePatchStorage {
    NativeSchemaStorage settings;
    std::vector<FC_StringView> depends_on;
    std::vector<FC_StringView> includes;
    std::vector<std::unique_ptr<NativeSupportStorage>> support_storage;
    std::vector<FC_SupportDefinition> supports;
    FC_PatchDefinition native{};
};

struct RegistrationState;

} // namespace detail

namespace detail {

// Patch models retain typed author definitions while exposing uniform metadata and native lowering operations.
struct PatchConcept {
    virtual ~PatchConcept() = default;
    [[nodiscard]] virtual std::string_view id() const noexcept = 0;
    [[nodiscard]] virtual std::optional<std::string_view> category() const noexcept = 0;
    [[nodiscard]] virtual std::unique_ptr<detail::NativePatchStorage>
    lower(const FC_HostApi* host, std::vector<std::shared_ptr<detail::HandlerAdapterBase>>& adapters) const = 0;
};

template <class Handler> class PatchModel final : public PatchConcept {
  public:
    using Settings = SettingsFor<Handler>;

    explicit PatchModel(PatchDefinition<Settings> definition, std::shared_ptr<const void> seed = {})
        : definition_(std::move(definition)), seed_(std::move(seed)) {}

    [[nodiscard]] std::string_view id() const noexcept override {
        return definition_.id;
    }

    [[nodiscard]] std::optional<std::string_view> category() const noexcept override {
        if (!definition_.category) {
            return std::nullopt;
        }
        return *definition_.category;
    }

    [[nodiscard]] std::unique_ptr<NativePatchStorage>
    lower(const FC_HostApi* host, std::vector<std::shared_ptr<HandlerAdapterBase>>& adapters) const override {
        if (definition_.supports.empty()) {
            throw std::invalid_argument{"A patch must declare at least one support"};
        }
        // Lower common settings and relationships before resolving inheritance for individual supports.
        auto result = std::make_unique<NativePatchStorage>();
        if (definition_.settings) {
            result->settings.append(*definition_.settings);
        }
        for (const auto& dependency : definition_.depends_on) {
            result->depends_on.push_back(detail::string_view(dependency));
        }
        for (const auto& included : definition_.includes) {
            result->includes.push_back(detail::string_view(included));
        }

        // Each support selects its effective handler/schema pair and retains the resulting callback adapter.
        result->support_storage.reserve(definition_.supports.size());
        result->supports.reserve(definition_.supports.size());
        for (const auto& support : definition_.supports) {
            if (support.implementation_ == nullptr) {
                throw std::invalid_argument{"A patch contains an empty support"};
            }
            auto storage = std::make_unique<NativeSupportStorage>();
            const auto& metadata = support.implementation_->metadata();
            FC_TargetLayout layouts = 0;
            for (const auto layout : metadata.layouts) {
                layouts |= static_cast<FC_TargetLayout>(layout);
            }
            for (const auto& dependency : metadata.depends_on) {
                storage->depends_on.push_back(detail::string_view(dependency));
            }
            for (const auto& included : metadata.includes) {
                storage->includes.push_back(detail::string_view(included));
            }
            support.implementation_->lower_settings(storage->settings);

            AdapterSelection selection;
            if (support.implementation_->inherits_handler()) {
                if constexpr (!std::same_as<Settings, NoSettings>) {
                    if (support.implementation_->has_settings()) {
                        throw std::invalid_argument{
                            "An inherited typed support cannot replace settings with NoSettings"};
                    }
                    if (!definition_.settings) {
                        throw std::invalid_argument{
                            "A typed patch handler requires a common settings schema for an inherited support"};
                    }
                }
                selection.adapter = std::make_shared<HandlerAdapter<Handler>>(host, definition_.settings, seed_);
            } else {
                selection = support.implementation_->select(host, type_token<Settings>(),
                                                            definition_.settings ? &*definition_.settings : nullptr);
            }
            if (!selection.adapter) {
                throw std::invalid_argument{"A support could not select its handler adapter"};
            }
            auto callbacks = selection.adapter->callbacks();
            adapters.push_back(std::move(selection.adapter));

            storage->native = {.layouts = layouts,
                               .roles = static_cast<FC_HostRole>(metadata.roles),
                               .image = static_cast<FC_TargetImage>(metadata.image),
                               .callbacks = callbacks,
                               .has_settings = support.implementation_->has_settings() ? FC_TRUE : FC_FALSE,
                               .settings = storage->settings.definitions.data(),
                               .setting_count = static_cast<std::uint32_t>(storage->settings.definitions.size()),
                               .depends_on = storage->depends_on.data(),
                               .depends_on_count = static_cast<std::uint32_t>(storage->depends_on.size()),
                               .includes = storage->includes.data(),
                               .include_count = static_cast<std::uint32_t>(storage->includes.size()),
                               .failure_policy = metadata.failure_policy
                                                     ? static_cast<FC_FailurePolicy>(*metadata.failure_policy)
                                                     : FC_FAILURE_INHERIT};
            result->supports.push_back(storage->native);
            result->support_storage.push_back(std::move(storage));
        }

        // Assemble the flat ABI record only after every backing vector has reached its final size.
        result->native = {.id = detail::string_view(definition_.id),
                          .name = detail::string_view(definition_.name),
                          .description = detail::optional_string_view(definition_.description),
                          .version = detail::optional_string_view(definition_.version),
                          .author = detail::optional_string_view(definition_.author),
                          .source = detail::optional_string_view(definition_.source),
                          .configurable = definition_.configurable ? FC_TRUE : FC_FALSE,
                          .enabled = definition_.enabled ? FC_TRUE : FC_FALSE,
                          .category = detail::optional_string_view(definition_.category),
                          .failure_policy = static_cast<FC_FailurePolicy>(definition_.failure_policy),
                          .settings = result->settings.definitions.data(),
                          .setting_count = static_cast<std::uint32_t>(result->settings.definitions.size()),
                          .depends_on = result->depends_on.data(),
                          .depends_on_count = static_cast<std::uint32_t>(result->depends_on.size()),
                          .includes = result->includes.data(),
                          .include_count = static_cast<std::uint32_t>(result->includes.size()),
                          .supports = result->supports.data(),
                          .support_count = static_cast<std::uint32_t>(result->supports.size())};
        return result;
    }

  private:
    PatchDefinition<Settings> definition_;
    std::shared_ptr<const void> seed_;
};

} // namespace detail

// Retains the handler model for later instance creation and rejects malformed author composition immediately.
template <class Handler> Patch patch(PatchDefinition<detail::SettingsFor<Handler>> definition) {
    detail::validate_patch_definition(definition);
    return Patch{std::make_shared<detail::PatchModel<Handler>>(std::move(definition))};
}

namespace detail {

template <class Function> struct PlanCallableTraits : PlanCallableTraits<decltype(&Function::operator())> {};

template <class Class> struct PlanCallableTraits<void (Class::*)(Plan&) const> {
    using Settings = NoSettings;
};

template <class Class>
struct PlanCallableTraits<void (Class::*)(Plan&) const noexcept> : PlanCallableTraits<void (Class::*)(Plan&) const> {};

template <class Class, class SettingsType>
struct PlanCallableTraits<void (Class::*)(Plan&, const SettingsType&) const> {
    using Settings = SettingsType;
};

template <class Class, class SettingsType>
struct PlanCallableTraits<void (Class::*)(Plan&, const SettingsType&) const noexcept>
    : PlanCallableTraits<void (Class::*)(Plan&, const SettingsType&) const> {};

template <class Function> using PlanSettingsFor = typename PlanCallableTraits<std::remove_cvref_t<Function>>::Settings;

template <class Function> class PlanOnlyHandler {
  public:
    using Settings = PlanSettingsFor<Function>;

    PlanOnlyHandler(const CreateContext&, std::unique_ptr<Settings> settings, std::shared_ptr<const void> function)
        requires(!std::same_as<Settings, NoSettings>)
        : function_(std::static_pointer_cast<const Function>(std::move(function))), settings_(std::move(settings)) {}

    PlanOnlyHandler(const CreateContext&, std::shared_ptr<const void> function)
        requires std::same_as<Settings, NoSettings>
        : function_(std::static_pointer_cast<const Function>(std::move(function))) {}

    void plan(Plan& plan) {
        if constexpr (std::same_as<Settings, NoSettings>) {
            std::invoke(*function_, plan);
        } else {
            std::invoke(*function_, plan, *settings_);
        }
    }

  private:
    std::shared_ptr<const Function> function_;
    std::unique_ptr<Settings> settings_;
};

} // namespace detail

// Adapts a callable retained only for the Plan callback to the same lifecycle and validation path as a full handler.
template <class PlanFunction>
Patch plan_patch(PatchDefinition<detail::PlanSettingsFor<PlanFunction>> definition, PlanFunction plan_function) {
    using Function = std::remove_cvref_t<PlanFunction>;
    static_assert(std::is_move_constructible_v<Function>, "A callable retained for the Plan callback must be movable");
    using Handler = detail::PlanOnlyHandler<Function>;
    detail::validate_patch_definition(definition);
    auto function = std::make_shared<const Function>(std::move(plan_function));
    return Patch{std::make_shared<detail::PatchModel<Handler>>(std::move(definition), std::move(function))};
}

namespace detail {

struct NativeGroupStorage {
    std::vector<FC_StringView> members;
    FC_GroupDefinition native{};
};

// Owns the author model, callback adapters, and native pointer graph for one accepted factory registration.
struct RegistrationState {
    Plugin plugin;
    std::vector<std::shared_ptr<HandlerAdapterBase>> adapters;
    std::vector<FC_CategoryDefinition> categories;
    std::vector<std::unique_ptr<NativeGroupStorage>> group_storage;
    std::vector<FC_GroupDefinition> groups;
    std::vector<std::unique_ptr<NativePatchStorage>> patch_storage;
    std::vector<FC_PatchDefinition> patches;
    FC_PluginDefinition native{};
};

struct PluginAccess {
    static const PluginDefinition& definition(const Plugin& plugin) {
        if (plugin.definition_ == nullptr) {
            throw std::invalid_argument{"The plugin factory returned an empty Plugin"};
        }
        return *plugin.definition_;
    }

    static void validate(const PluginDefinition& definition) {
        require_id(definition.id, "A plugin ID");
        if (reserved_plugin_id(definition.id)) {
            invalid_composition("FusionCutter is a reserved plugin ID");
        }
        if (definition.patches.empty() && !equal_ascii_case_insensitive(definition.id, "Core")) {
            invalid_composition("A plugin must declare at least one patch");
        }

        // A single catalog namespace prevents ambiguous category, group, and patch references.
        std::vector<std::string_view> category_ids;
        std::vector<std::string_view> patch_ids;
        std::vector<std::string_view> catalog_ids;
        const auto add_catalog_id = [&](std::string_view id, std::string_view subject) {
            require_id(id, subject);
            if (reserved_catalog_id(id)) {
                throw std::invalid_argument{std::string{"FusionCutter and General are reserved "} +
                                            std::string{subject}};
            }
            if (std::ranges::any_of(catalog_ids, [&](std::string_view existing) {
                    return equal_ascii_case_insensitive(existing, id);
                })) {
                invalid_composition("Patch, group, and category IDs must be distinct within one plugin");
            }
            catalog_ids.push_back(id);
        };

        for (const auto& category : definition.categories) {
            add_catalog_id(category.id, "category IDs");
            category_ids.push_back(category.id);
        }
        for (const auto& group : definition.groups) {
            add_catalog_id(group.id, "group IDs");
            if (!group.configurable && group.enabled) {
                invalid_composition("A nonconfigurable group cannot declare enabled=true");
            }
            if (group.category) {
                require_id(*group.category, "A group category ID");
            }
            for (std::size_t index = 0; index < group.members.size(); ++index) {
                require_id(group.members[index], "A group member ID");
                for (std::size_t prior = 0; prior < index; ++prior) {
                    if (equal_ascii_case_insensitive(group.members[prior], group.members[index])) {
                        invalid_composition("A group cannot repeat a patch member");
                    }
                }
            }
        }
        for (const auto& patch : definition.patches) {
            if (!patch.implementation_) {
                invalid_composition("A plugin contains an empty Patch");
            }
            add_catalog_id(patch.implementation_->id(), "patch IDs");
            patch_ids.push_back(patch.implementation_->id());
        }

        // References are checked only after the complete owned ID sets are known.
        const auto has_category = [&](std::string_view id) {
            return std::ranges::any_of(category_ids, [&](std::string_view existing) {
                return equal_ascii_case_insensitive(existing, id);
            });
        };
        for (const auto& group : definition.groups) {
            if (group.category && !has_category(*group.category)) {
                invalid_composition("A group references a category not declared by its plugin");
            }
        }
        for (const auto& patch : definition.patches) {
            const auto category = patch.implementation_->category();
            if (category && !has_category(*category)) {
                invalid_composition("A patch references a category not declared by its plugin");
            }
        }

        // Group membership is exclusive so one patch cannot receive conflicting presentation toggles.
        std::vector<std::string_view> grouped_patch_ids;
        for (const auto& group : definition.groups) {
            for (const auto& member : group.members) {
                const auto owned = std::ranges::any_of(patch_ids, [&](std::string_view patch_id) {
                    return equal_ascii_case_insensitive(patch_id, member);
                });
                if (!owned) {
                    invalid_composition("A group member must name a patch owned by its plugin");
                }
                if (std::ranges::any_of(grouped_patch_ids, [&](std::string_view existing) {
                        return equal_ascii_case_insensitive(existing, member);
                    })) {
                    invalid_composition("A patch can belong to at most one group");
                }
                grouped_patch_ids.push_back(member);
            }
        }
    }

    static std::unique_ptr<RegistrationState> lower(Plugin plugin, const FC_HostApi* host) {
        auto state = std::make_unique<RegistrationState>(RegistrationState{.plugin = std::move(plugin)});
        const auto& definition = PluginAccess::definition(state->plugin);
        validate(definition);
        // Lower each level into owned backing storage before publishing pointers from its native record.
        state->categories.reserve(definition.categories.size());
        for (const auto& category : definition.categories) {
            state->categories.push_back({.id = detail::string_view(category.id),
                                         .has_order = category.order ? FC_TRUE : FC_FALSE,
                                         .order = category.order.value_or(0)});
        }

        state->group_storage.reserve(definition.groups.size());
        state->groups.reserve(definition.groups.size());
        for (const auto& group : definition.groups) {
            auto storage = std::make_unique<NativeGroupStorage>();
            for (const auto& member : group.members) {
                storage->members.push_back(detail::string_view(member));
            }
            storage->native = {.id = detail::string_view(group.id),
                               .members = storage->members.data(),
                               .member_count = static_cast<std::uint32_t>(storage->members.size()),
                               .configurable = group.configurable ? FC_TRUE : FC_FALSE,
                               .enabled = group.enabled ? FC_TRUE : FC_FALSE,
                               .category = detail::optional_string_view(group.category),
                               .description = detail::optional_string_view(group.description)};
            state->groups.push_back(storage->native);
            state->group_storage.push_back(std::move(storage));
        }

        state->patch_storage.reserve(definition.patches.size());
        state->patches.reserve(definition.patches.size());
        for (const auto& patch : definition.patches) {
            if (patch.implementation_ == nullptr) {
                throw std::invalid_argument{"The plugin contains an empty Patch"};
            }
            auto storage = patch.implementation_->lower(host, state->adapters);
            state->patches.push_back(storage->native);
            state->patch_storage.push_back(std::move(storage));
        }

        // The top-level view is assembled last so all nested pointer targets are stable for synchronous submission.
        state->native = {.struct_size = sizeof(FC_PluginDefinition),
                         .id = detail::string_view(definition.id),
                         .version = detail::optional_string_view(definition.version),
                         .author = detail::optional_string_view(definition.author),
                         .source = detail::optional_string_view(definition.source),
                         .categories = state->categories.data(),
                         .category_count = static_cast<std::uint32_t>(state->categories.size()),
                         .groups = state->groups.data(),
                         .group_count = static_cast<std::uint32_t>(state->groups.size()),
                         .patches = state->patches.data(),
                         .patch_count = static_cast<std::uint32_t>(state->patches.size())};
        return state;
    }
};

// Each factory has one retained state slot so copied definitions never outlive their callback objects.
template <auto Factory> [[nodiscard]] std::unique_ptr<RegistrationState>& registration_state() noexcept {
    static std::unique_ptr<RegistrationState> state;
    return state;
}

template <auto Factory>
FC_CallStatus FC_CALL register_factory(const FC_HostApi* host, const FC_RegistrySink* registry,
                                       const FC_ErrorSink* error) {
    // The factory boundary contains every author exception and retains native backing state only after submission.
    auto& admitted = registration_state<Factory>();
    if (admitted != nullptr) {
        set_error(error, "The plugin factory was registered more than once", "Register plugin");
        return FC_CALL_FAILED;
    }
    if (host == nullptr || host->struct_size < sizeof(FC_HostApi) || registry == nullptr ||
        registry->struct_size < sizeof(FC_RegistrySink) || registry->submit == nullptr) {
        set_error(error, "The host supplied malformed registration tables", "Register plugin");
        return FC_CALL_FAILED;
    }
    try {
        auto candidate = PluginAccess::lower(std::invoke(Factory), host);
        if (registry->submit(registry->context, &candidate->native) != FC_SUBMIT_ACCEPTED) {
            set_error(error, "The registry rejected the plugin definition", "Submit plugin definition");
            return FC_CALL_FAILED;
        }
        admitted = std::move(candidate);
        return FC_CALL_OK;
    } catch (const std::exception& exception) {
        set_error(error, exception.what(), "Register plugin");
    } catch (...) {
        set_error(error, "The plugin factory threw an unknown exception", "Register plugin");
    }
    return FC_CALL_FAILED;
}

// The framework calls this only after destroying every copied definition that can reference retained callbacks.
template <auto Factory> void release_registration() noexcept {
    registration_state<Factory>().reset();
}

template <auto Factory> [[nodiscard]] const FC_PluginApi* query_plugin(std::uint32_t generation) noexcept {
    static const FC_PluginApi api{.struct_size = sizeof(FC_PluginApi),
                                  .sdk_revision = FC_SDK_REVISION,
                                  .host_api_size = sizeof(FC_HostApi),
                                  .register_plugin = &register_factory<Factory>};
    return generation == FC_PLUGIN_ABI_GENERATION ? &api : nullptr;
}

template <auto Factory> [[nodiscard]] constexpr FC_RegisterPluginFn bundled_registration() noexcept {
    return &register_factory<Factory>;
}

} // namespace detail

} // namespace fc

#if defined(_M_IX86)
#define FC_DETAIL_EXPORT_PLUGIN_QUERY                                                                                  \
    __pragma(comment(linker, "/export:FusionCutter_QueryPlugin=_FusionCutter_QueryPlugin"))
#elif defined(_M_X64)
#define FC_DETAIL_EXPORT_PLUGIN_QUERY __pragma(comment(linker, "/export:FusionCutter_QueryPlugin"))
#else
#error Fusion Cutter supports only x86 and x64 plugin builds.
#endif

#define FC_EXPORT_PLUGIN(factory)                                                                                      \
    FC_DETAIL_EXPORT_PLUGIN_QUERY                                                                                      \
    FC_EXTERN_C const FC_PluginApi* FC_CALL FusionCutter_QueryPlugin(uint32_t abi_generation) FC_NOEXCEPT {            \
        return ::fc::detail::query_plugin<&factory>(abi_generation);                                                   \
    }

namespace fc {

namespace detail {

// Planning traits lower author locations, values, and callback shapes into the stable sink request vocabulary.
// Stores both halves of an explicit call-through so publication never exposes a raw target without its ABI adapter.
template <class Call> struct OriginalBinding {
    std::atomic<std::uintptr_t> address{};
    std::atomic<std::uintptr_t> invoker{};
};

// One process-lifetime SDK thunk converts a logical C++ call into each explicit physical NativeCall layout.
template <class Call> [[nodiscard]] std::uintptr_t native_invoker() noexcept;

// Names the compiler-facing function pointer shapes used to enter a reverse adapter without casting the raw target.
template <class Signature> struct LogicalFunctionPointer;

template <class Result, class... Args> struct LogicalFunctionPointer<Result(Args...)> {
    using type = Result(FC_CALL*)(Args...) noexcept;
    using invoker = Result(FC_CALL*)(std::uintptr_t, Args...) noexcept;
};

// Wraps an explicit physical NativeCall address without presenting it as a function pointer the compiler can call.
template <class Call> class NativeCallable {
  public:
    NativeCallable() noexcept = default;
    explicit NativeCallable(std::uintptr_t address) noexcept : address_(address), invoker_(native_invoker<Call>()) {}

    NativeCallable(std::uintptr_t address, std::uintptr_t invoker) noexcept : address_(address), invoker_(invoker) {}

    [[nodiscard]] explicit operator bool() const noexcept {
        return address_ != 0 && invoker_ != 0;
    }

    template <class... Args> decltype(auto) operator()(Args&&... arguments) const noexcept {
        using Invoker = typename LogicalFunctionPointer<typename FunctionTraits<Call>::signature>::invoker;
        assert(address_ != 0 && invoker_ != 0);
        return reinterpret_cast<Invoker>(invoker_)(address_, std::forward<Args>(arguments)...);
    }

  private:
    std::uintptr_t address_{};
    std::uintptr_t invoker_{};
};

template <class T> void start_allocation_lifetime(void* address, std::size_t count) noexcept {
#if defined(__cpp_lib_start_lifetime_as) && __cpp_lib_start_lifetime_as >= 202207L
    (void)std::start_lifetime_as_array<T>(address, count);
#else
    // MSVC's C++23 library does not yet expose std::start_lifetime_as_array. memmove is the standard implicit-lifetime
    // operation used here without changing the framework-initialized object representation.
    (void)std::memmove(address, address, count * sizeof(T));
#endif
}

template <class T> struct LocationKind;

template <class T, std::size_t Count>
struct LocationKind<DataLocation<T, Count>> : std::integral_constant<FC_LocationKind, FC_LOCATION_DATA> {};

template <class Call>
struct LocationKind<FunctionLocation<Call>> : std::integral_constant<FC_LocationKind, FC_LOCATION_FUNCTION> {};

template <class Call>
struct LocationKind<CallLocation<Call>> : std::integral_constant<FC_LocationKind, FC_LOCATION_CODE> {};

template <> struct LocationKind<CodeLocation> : std::integral_constant<FC_LocationKind, FC_LOCATION_CODE> {};

template <> struct LocationKind<VtableLocation> : std::integral_constant<FC_LocationKind, FC_LOCATION_DATA> {};

// Centralized access keeps every public Plan overload on the same evidence and location conversion path.
struct PlanAccess {
    static FC_Evidence evidence(const Evidence& value) noexcept {
        return {.kind = value.kind_,
                .bytes = byte_view(value.bytes_),
                .mask = byte_view(value.mask_),
                .target_rva = value.target_rva_};
    }

    template <class Location> static FC_LocationView location(const Location& value) noexcept {
        return {.kind = LocationKind<std::remove_cvref_t<Location>>::value,
                .rva = value.rva.value,
                .name = string_view(value.name),
                .label = string_view(value.label),
                .evidence = evidence(value.evidence)};
    }

    static FC_LocationView compact(Rva rva, FC_LocationKind kind, const Evidence& value) noexcept {
        return {.kind = kind, .rva = rva.value, .evidence = evidence(value)};
    }

    static FC_AddressTarget data_address(const DataAddress& value) noexcept {
        return {.kind = FC_ADDRESS_DATA, .data = value.handle_, .data_offset = value.offset_};
    }
};

template <class T> struct IsAddressExpression : std::false_type {};

template <class Target, FC_WriteKind Kind>
struct IsAddressExpression<AddressExpression<Target, Kind>> : std::true_type {};

template <class T> inline constexpr bool is_address_expression_v = IsAddressExpression<std::remove_cvref_t<T>>::value;

template <class T> struct IsDataLocation : std::false_type {};

template <class Value, std::size_t Count> struct IsDataLocation<DataLocation<Value, Count>> : std::true_type {};

template <class T> struct IsFunctionLocation : std::false_type {};

template <class Call> struct IsFunctionLocation<FunctionLocation<Call>> : std::true_type {};

template <class T>
concept ImageLocation =
    IsDataLocation<std::remove_cvref_t<T>>::value || IsFunctionLocation<std::remove_cvref_t<T>>::value ||
    std::same_as<std::remove_cvref_t<T>, CodeLocation> || std::same_as<std::remove_cvref_t<T>, VtableLocation>;

template <class T>
concept ContiguousNativeRange =
    std::ranges::contiguous_range<T> && std::ranges::sized_range<T> && NativeData<std::ranges::range_value_t<T>>;

template <class T> [[nodiscard]] std::span<const std::byte> replacement_bytes(const T& value) noexcept {
    if constexpr (ContiguousNativeRange<T>) {
        return std::as_bytes(std::span{std::ranges::data(value), std::ranges::size(value)});
    } else {
        static_assert(NativeData<T>, "Plan::write requires a trivially copyable value or contiguous value range");
        return std::as_bytes(std::span{&value, std::size_t{1}});
    }
}

template <class Target> [[nodiscard]] FC_AddressTarget address_target(const Target& target) noexcept {
    using Clean = std::remove_cvref_t<Target>;
    if constexpr (std::same_as<Clean, DataAddress>) {
        return PlanAccess::data_address(target);
    } else if constexpr (ImageLocation<Clean>) {
        return {.kind = FC_ADDRESS_IMAGE, .image_rva = target.rva.value};
    } else if constexpr (NativeFunction<Clean>) {
        return {.kind = FC_ADDRESS_PLUGIN_FUNCTION, .plugin_function = reinterpret_cast<std::uintptr_t>(target)};
    } else {
        static_assert(dependent_false_v<Target>,
                      "A Plan operation target must be an image location, DataAddress, or native function pointer");
    }
}

inline constexpr std::uint32_t kGeneratedHookEntrySize = 512;

// Captures both ambient Windows error APIs because observer code must be invisible outside its explicit arguments.
struct AmbientErrorState {
    DWORD windows{};
    int winsock{};
};

[[nodiscard]] inline AmbientErrorState capture_ambient_errors() noexcept {
    const auto windows = GetLastError();
    const auto winsock = WSAGetLastError();
    return {windows, winsock};
}

inline void restore_ambient_errors(AmbientErrorState state) noexcept {
    WSASetLastError(state.winsock);
    SetLastError(state.windows);
}

// Writes one bounded site entry without retaining the callback-scoped build packet or allocating executable memory.
class HookEntryWriter {
  public:
    explicit HookEntryWriter(std::span<std::uint8_t> output) noexcept : output_(output) {}

    [[nodiscard]] bool append(std::initializer_list<std::uint8_t> bytes) noexcept {
        if (bytes.size() > output_.size() - offset_) {
            return false;
        }
        std::ranges::copy(bytes, output_.begin() + static_cast<std::ptrdiff_t>(offset_));
        offset_ += bytes.size();
        return true;
    }

    template <class T> [[nodiscard]] bool append_value(T value) noexcept {
        static_assert(std::is_trivially_copyable_v<T>);
        if (sizeof(T) > output_.size() - offset_) {
            return false;
        }
        std::memcpy(output_.data() + offset_, &value, sizeof(value));
        offset_ += sizeof(value);
        return true;
    }

  private:
    std::span<std::uint8_t> output_;
    std::size_t offset_{};
};

[[nodiscard]] constexpr std::uint32_t hook_align_up(std::uint32_t value, std::uint32_t alignment) noexcept {
    return (value + alignment - 1) & ~(alignment - 1);
}

#if defined(_M_X64)

// Maps reviewed ABI homes to encoder register numbers; the emitters below use only stack-relative fixed encodings.
[[nodiscard]] constexpr std::optional<std::uint8_t> x64_general_register(FC_NativeRegister value) noexcept {
    switch (value) {
    case FC_REGISTER_RAX:
        return std::uint8_t{0};
    case FC_REGISTER_RCX:
        return std::uint8_t{1};
    case FC_REGISTER_RDX:
        return std::uint8_t{2};
    case FC_REGISTER_RBX:
        return std::uint8_t{3};
    case FC_REGISTER_RBP:
        return std::uint8_t{5};
    case FC_REGISTER_RSI:
        return std::uint8_t{6};
    case FC_REGISTER_RDI:
        return std::uint8_t{7};
    case FC_REGISTER_R8:
        return std::uint8_t{8};
    case FC_REGISTER_R9:
        return std::uint8_t{9};
    case FC_REGISTER_R10:
        return std::uint8_t{10};
    case FC_REGISTER_R11:
        return std::uint8_t{11};
    case FC_REGISTER_R12:
        return std::uint8_t{12};
    case FC_REGISTER_R13:
        return std::uint8_t{13};
    case FC_REGISTER_R14:
        return std::uint8_t{14};
    case FC_REGISTER_R15:
        return std::uint8_t{15};
    default:
        return std::nullopt;
    }
}

[[nodiscard]] constexpr std::optional<std::uint8_t> x64_simd_register(FC_NativeRegister value) noexcept {
    if (value < FC_REGISTER_XMM0 || value > FC_REGISTER_XMM15) {
        return std::nullopt;
    }
    return static_cast<std::uint8_t>(value - FC_REGISTER_XMM0);
}

[[nodiscard]] inline bool emit_x64_stack_adjust(HookEntryWriter& writer, bool subtract, std::uint32_t amount) noexcept {
    if (amount <= std::numeric_limits<std::uint8_t>::max()) {
        return writer.append(
            {0x48, 0x83, static_cast<std::uint8_t>(subtract ? 0xec : 0xc4), static_cast<std::uint8_t>(amount)});
    }
    return writer.append({0x48, 0x81, static_cast<std::uint8_t>(subtract ? 0xec : 0xc4)}) &&
           writer.append_value(amount);
}

[[nodiscard]] inline bool emit_x64_store_general(HookEntryWriter& writer, std::uint8_t source,
                                                 std::uint32_t displacement) noexcept {
    const auto rex = static_cast<std::uint8_t>(0x48 | (source >= 8 ? 0x04 : 0));
    const auto modrm = static_cast<std::uint8_t>(0x84 | ((source & 7) << 3));
    return writer.append({rex, 0x89, modrm, 0x24}) && writer.append_value(displacement);
}

[[nodiscard]] inline bool emit_x64_load_general(HookEntryWriter& writer, std::uint8_t destination,
                                                std::uint32_t displacement) noexcept {
    const auto rex = static_cast<std::uint8_t>(0x48 | (destination >= 8 ? 0x04 : 0));
    const auto modrm = static_cast<std::uint8_t>(0x84 | ((destination & 7) << 3));
    return writer.append({rex, 0x8b, modrm, 0x24}) && writer.append_value(displacement);
}

[[nodiscard]] inline bool emit_x64_load_address(HookEntryWriter& writer, std::uint8_t destination,
                                                std::uint32_t displacement) noexcept {
    const auto rex = static_cast<std::uint8_t>(0x48 | (destination >= 8 ? 0x04 : 0));
    const auto modrm = static_cast<std::uint8_t>(0x84 | ((destination & 7) << 3));
    return writer.append({rex, 0x8d, modrm, 0x24}) && writer.append_value(displacement);
}

[[nodiscard]] inline bool emit_x64_store_simd(HookEntryWriter& writer, std::uint8_t source, bool is_double,
                                              std::uint32_t displacement) noexcept {
    if (!writer.append({static_cast<std::uint8_t>(is_double ? 0xf2 : 0xf3)}) ||
        (source >= 8 && !writer.append({static_cast<std::uint8_t>(0x44)}))) {
        return false;
    }
    const auto modrm = static_cast<std::uint8_t>(0x84 | ((source & 7) << 3));
    return writer.append({0x0f, 0x11, modrm, 0x24}) && writer.append_value(displacement);
}

[[nodiscard]] inline bool emit_x64_load_simd(HookEntryWriter& writer, std::uint8_t destination, bool is_double,
                                             std::uint32_t displacement) noexcept {
    if (!writer.append({static_cast<std::uint8_t>(is_double ? 0xf2 : 0xf3)}) ||
        (destination >= 8 && !writer.append({static_cast<std::uint8_t>(0x44)}))) {
        return false;
    }
    const auto modrm = static_cast<std::uint8_t>(0x84 | ((destination & 7) << 3));
    return writer.append({0x0f, 0x10, modrm, 0x24}) && writer.append_value(displacement);
}

// Full-width saves preserve Windows nonvolatile SIMD registers that an explicit layout temporarily repurposes.
[[nodiscard]] inline bool emit_x64_store_simd_full(HookEntryWriter& writer, std::uint8_t source,
                                                   std::uint32_t displacement) noexcept {
    if (!writer.append({0xf3}) || (source >= 8 && !writer.append({static_cast<std::uint8_t>(0x44)}))) {
        return false;
    }
    const auto modrm = static_cast<std::uint8_t>(0x84 | ((source & 7) << 3));
    return writer.append({0x0f, 0x7f, modrm, 0x24}) && writer.append_value(displacement);
}

[[nodiscard]] inline bool emit_x64_load_simd_full(HookEntryWriter& writer, std::uint8_t destination,
                                                  std::uint32_t displacement) noexcept {
    if (!writer.append({0xf3}) || (destination >= 8 && !writer.append({static_cast<std::uint8_t>(0x44)}))) {
        return false;
    }
    const auto modrm = static_cast<std::uint8_t>(0x84 | ((destination & 7) << 3));
    return writer.append({0x0f, 0x6f, modrm, 0x24}) && writer.append_value(displacement);
}

[[nodiscard]] inline bool emit_x64_copy_stack(HookEntryWriter& writer, std::uint32_t source, std::uint32_t destination,
                                              std::uint32_t size) noexcept {
    // Moving 8, 4, 2, then 1 byte copies the exact tail without reading beyond a small record's declared extent.
    std::uint32_t copied = 0;
    while (size - copied >= 8) {
        if (!emit_x64_load_general(writer, 0, source + copied) ||
            !emit_x64_store_general(writer, 0, destination + copied)) {
            return false;
        }
        copied += 8;
    }
    if (size - copied >= 4) {
        if (!writer.append({0x8b, 0x84, 0x24}) || !writer.append_value(source + copied) ||
            !writer.append({0x89, 0x84, 0x24}) || !writer.append_value(destination + copied)) {
            return false;
        }
        copied += 4;
    }
    if (size - copied >= 2) {
        if (!writer.append({0x66, 0x8b, 0x84, 0x24}) || !writer.append_value(source + copied) ||
            !writer.append({0x66, 0x89, 0x84, 0x24}) || !writer.append_value(destination + copied)) {
            return false;
        }
        copied += 2;
    }
    if (size != copied) {
        return writer.append({0x8a, 0x84, 0x24}) && writer.append_value(source + copied) &&
               writer.append({0x88, 0x84, 0x24}) && writer.append_value(destination + copied);
    }
    return true;
}

// Copies an indirectly passed record into the physical value spill consumed by explicit argument homes.
[[nodiscard]] inline bool emit_x64_copy_pointer_to_stack(HookEntryWriter& writer, std::uint32_t pointer_offset,
                                                         std::uint32_t destination, std::uint32_t size) noexcept {
    if (!emit_x64_load_general(writer, 2, pointer_offset)) {
        return false;
    }
    std::uint32_t copied = 0;
    while (size - copied >= 8) {
        if (!writer.append({0x48, 0x8b, 0x82}) || !writer.append_value(copied) ||
            !emit_x64_store_general(writer, 0, destination + copied)) {
            return false;
        }
        copied += 8;
    }
    if (size - copied >= 4) {
        if (!writer.append({0x8b, 0x82}) || !writer.append_value(copied) || !writer.append({0x89, 0x84, 0x24}) ||
            !writer.append_value(destination + copied)) {
            return false;
        }
        copied += 4;
    }
    if (size - copied >= 2) {
        if (!writer.append({0x66, 0x8b, 0x82}) || !writer.append_value(copied) ||
            !writer.append({0x66, 0x89, 0x84, 0x24}) || !writer.append_value(destination + copied)) {
            return false;
        }
        copied += 2;
    }
    if (size != copied) {
        return writer.append({0x8a, 0x82}) && writer.append_value(copied) && writer.append({0x88, 0x84, 0x24}) &&
               writer.append_value(destination + copied);
    }
    return true;
}

[[nodiscard]] constexpr bool x64_logical_hidden_result(const FC_NativeValue& result) noexcept {
    return result.kind == FC_NATIVE_RECORD && result.size != 1 && result.size != 2 && result.size != 4 &&
           result.size != 8;
}

[[nodiscard]] constexpr bool x64_logical_indirect_argument(const FC_NativeValue& value) noexcept {
    return value.kind == FC_NATIVE_RECORD && value.size != 1 && value.size != 2 && value.size != 4 && value.size != 8;
}

// Stores a record result returned in compiler registers through the physical return pointer retained in the frame.
[[nodiscard]] inline bool emit_x64_store_rax_to_pointer(HookEntryWriter& writer, std::uint32_t pointer_offset,
                                                        std::uint32_t byte_size) noexcept {
    if (!emit_x64_load_general(writer, 11, pointer_offset)) {
        return false;
    }
    if (byte_size == 8) {
        return writer.append({0x49, 0x89, 0x03});
    }
    if (byte_size == 4) {
        return writer.append({0x41, 0x89, 0x03});
    }
    if (byte_size == 2) {
        return writer.append({0x66, 0x41, 0x89, 0x03});
    }
    return byte_size == 1 && writer.append({0x41, 0x88, 0x03});
}

// Generates the hook boundary: capture physical layout, call the typed dispatcher, and return through the game's
// declared result home without exposing the helper ABI to target code.
[[nodiscard]] inline bool emit_x64_typed_entry(HookEntryWriter& writer, const FC_NativeCall& call,
                                               std::uintptr_t snapshot_slot, std::uintptr_t dispatcher) noexcept {
    const auto logical_hidden_result = x64_logical_hidden_result(call.result);
    const auto helper_parameter_count = call.argument_count + 1U + static_cast<std::uint32_t>(logical_hidden_result);
    const auto helper_stack_size = helper_parameter_count <= 4 ? 0U : (helper_parameter_count - 4) * 8U;
    const auto helper_area = 32U + helper_stack_size;
    std::vector<std::uint32_t> spill_offsets;
    spill_offsets.reserve(call.argument_count);
    auto spill_size = hook_align_up(helper_area, 16);
    for (std::uint32_t index = 0; index < call.argument_count; ++index) {
        const auto& value = call.arguments[index].value;
        spill_size = hook_align_up(spill_size, std::max(1U, value.alignment));
        spill_offsets.push_back(spill_size);
        spill_size += value.size;
    }
    const auto return_pointer_offset = hook_align_up(spill_size, 8);
    if (call.result.kind == FC_NATIVE_RECORD) {
        spill_size = return_pointer_offset + 8;
    }
    // An x64 caller reserves shadow space and presents a 16-byte-aligned stack immediately before CALL.
    auto frame_size = hook_align_up(spill_size, 16) + 8;
    if (!emit_x64_stack_adjust(writer, true, frame_size)) {
        return false;
    }

    // Spill every register argument before RAX becomes the scratch register used for stack-resident values.
    for (std::uint32_t index = 0; index < call.argument_count; ++index) {
        const auto& argument = call.arguments[index];
        if (argument.storage.kind != FC_NATIVE_STORAGE_REGISTER) {
            continue;
        }
        if (const auto general = x64_general_register(argument.storage.register_id)) {
            if (!emit_x64_store_general(writer, *general, spill_offsets[index])) {
                return false;
            }
        } else if (const auto simd = x64_simd_register(argument.storage.register_id)) {
            if (!emit_x64_store_simd(writer, *simd, argument.value.kind == FC_NATIVE_FLOAT_64, spill_offsets[index])) {
                return false;
            }
        } else {
            return false;
        }
    }
    for (std::uint32_t index = 0; index < call.argument_count; ++index) {
        const auto& argument = call.arguments[index];
        if (argument.storage.kind == FC_NATIVE_STORAGE_STACK &&
            !emit_x64_copy_stack(writer, frame_size + 40 + argument.storage.stack_offset, spill_offsets[index],
                                 argument.value.size)) {
            return false;
        }
    }

    // A pointer to storage for a returned record is an input to this boundary even though it describes the output.
    if (call.result.kind == FC_NATIVE_RECORD) {
        if (call.return_storage.kind == FC_NATIVE_STORAGE_REGISTER) {
            const auto source = x64_general_register(call.return_storage.register_id);
            if (!source || !emit_x64_store_general(writer, *source, return_pointer_offset)) {
                return false;
            }
        } else if (!emit_x64_copy_stack(writer, frame_size + 40 + call.return_storage.stack_offset,
                                        return_pointer_offset, 8)) {
            return false;
        }
    }

    // Hidden logical returns occupy the compiler ABI's first position; the site slot and arguments shift together.
    constexpr std::array<std::uint8_t, 4> helper_general_registers{1, 2, 8, 9};
    if (logical_hidden_result && !emit_x64_load_general(writer, 1, return_pointer_offset)) {
        return false;
    }
    const auto slot_position = static_cast<std::uint32_t>(logical_hidden_result);
    if (slot_position == 0) {
        if (!writer.append({0x48, 0xb9}) || !writer.append_value(snapshot_slot)) {
            return false;
        }
    } else if (!writer.append({0x48, 0xba}) || !writer.append_value(snapshot_slot)) {
        return false;
    }
    for (std::uint32_t index = 0; index < call.argument_count; ++index) {
        const auto& value = call.arguments[index].value;
        const auto helper_position = index + 1 + static_cast<std::uint32_t>(logical_hidden_result);
        const auto indirect = x64_logical_indirect_argument(value);
        if (helper_position < 4) {
            if (indirect) {
                if (!emit_x64_load_address(writer, helper_general_registers[helper_position], spill_offsets[index])) {
                    return false;
                }
            } else if (value.kind == FC_NATIVE_FLOAT_32 || value.kind == FC_NATIVE_FLOAT_64) {
                if (!emit_x64_load_simd(writer, static_cast<std::uint8_t>(helper_position),
                                        value.kind == FC_NATIVE_FLOAT_64, spill_offsets[index])) {
                    return false;
                }
            } else if (!emit_x64_load_general(writer, helper_general_registers[helper_position],
                                              spill_offsets[index])) {
                return false;
            }
        } else {
            const auto helper_offset = 32 + (helper_position - 4) * 8;
            if (indirect) {
                if (!emit_x64_load_address(writer, 0, spill_offsets[index]) ||
                    !emit_x64_store_general(writer, 0, helper_offset)) {
                    return false;
                }
            } else if (!emit_x64_copy_stack(writer, spill_offsets[index], helper_offset, value.size)) {
                return false;
            }
        }
    }
    if (!writer.append({0x48, 0xb8}) || !writer.append_value(dispatcher) || !writer.append({0xff, 0xd0})) {
        return false;
    }

    // Convert the ordinary helper result back to the physical home described by the native call.
    if (call.result.kind == FC_NATIVE_RECORD && !logical_hidden_result) {
        if (!emit_x64_store_rax_to_pointer(writer, return_pointer_offset, call.result.size)) {
            return false;
        }
    } else if (call.result.kind != FC_NATIVE_VOID && call.result.kind != FC_NATIVE_RECORD &&
               call.return_storage.kind == FC_NATIVE_STORAGE_REGISTER) {
        if (const auto general = x64_general_register(call.return_storage.register_id)) {
            if (*general != 0) {
                const auto rex = static_cast<std::uint8_t>(0x48 | (*general >= 8 ? 0x01 : 0));
                if (!writer.append({rex, 0x89, static_cast<std::uint8_t>(0xc0 | (*general & 7))})) {
                    return false;
                }
            }
        } else if (const auto simd = x64_simd_register(call.return_storage.register_id)) {
            if (*simd != 0) {
                if ((*simd >= 8 && !writer.append({0x44})) ||
                    !writer.append({0x0f, 0x28, static_cast<std::uint8_t>(0xc0 | ((*simd & 7) << 3))})) {
                    return false;
                }
            }
        } else {
            return false;
        }
    }
    return emit_x64_stack_adjust(writer, false, frame_size) && writer.append({0xc3});
}

// Generates the reverse boundary used by resolved explicit callables and Original: logical Windows x64 in, the
// declared register/stack layout out, then the physical result back into the logical result home owned by the compiler.
[[nodiscard]] inline bool emit_x64_native_invoker(HookEntryWriter& writer, const FC_NativeCall& call) {
    const auto logical_hidden_result = x64_logical_hidden_result(call.result);
    constexpr std::array<std::uint8_t, 8> nonvolatile_general{3, 5, 6, 7, 12, 13, 14, 15};
    constexpr std::array<std::uint8_t, 10> nonvolatile_simd{6, 7, 8, 9, 10, 11, 12, 13, 14, 15};
    const auto align_cursor = [](std::uint64_t value, std::uint32_t alignment) {
        return (value + alignment - 1) & ~static_cast<std::uint64_t>(alignment - 1);
    };

    // The frame starts with the physical callee's shadow/stack area; all source values are spilled above it before
    // scratch registers or explicit argument homes can overwrite the logical call.
    std::uint64_t cursor = 32ULL + call.stack_size;
    cursor = align_cursor(cursor, 8);
    const auto target_offset = static_cast<std::uint32_t>(cursor);
    cursor += 8;
    std::vector<std::uint32_t> argument_offsets;
    std::vector<std::uint32_t> argument_pointer_offsets;
    argument_offsets.reserve(call.argument_count);
    argument_pointer_offsets.reserve(call.argument_count);
    for (std::uint32_t index = 0; index < call.argument_count; ++index) {
        const auto& value = call.arguments[index].value;
        cursor = align_cursor(cursor, std::max(1U, value.alignment));
        argument_offsets.push_back(static_cast<std::uint32_t>(cursor));
        cursor += std::max(8U, value.size);
        cursor = align_cursor(cursor, 8);
        argument_pointer_offsets.push_back(static_cast<std::uint32_t>(cursor));
        cursor += 8;
    }
    cursor = align_cursor(cursor, std::max(8U, call.result.alignment));
    const auto result_offset = static_cast<std::uint32_t>(cursor);
    cursor += std::max(8U, call.result.size);
    cursor = align_cursor(cursor, 8);
    const auto logical_result_pointer_offset = static_cast<std::uint32_t>(cursor);
    cursor += 8;
    std::array<std::uint32_t, nonvolatile_general.size()> general_offsets{};
    for (auto& offset : general_offsets) {
        offset = static_cast<std::uint32_t>(cursor);
        cursor += 8;
    }
    cursor = align_cursor(cursor, 16);
    std::array<std::uint32_t, nonvolatile_simd.size()> simd_offsets{};
    for (auto& offset : simd_offsets) {
        offset = static_cast<std::uint32_t>(cursor);
        cursor += 16;
    }
    const auto aligned = align_cursor(cursor, 16);
    if (aligned > std::numeric_limits<std::uint32_t>::max() - 8) {
        return false;
    }
    const auto frame_size = static_cast<std::uint32_t>(aligned + 8);
    if (!emit_x64_stack_adjust(writer, true, frame_size)) {
        return false;
    }

    constexpr std::array<std::uint8_t, 4> logical_general{1, 2, 8, 9};
    if (logical_hidden_result && !emit_x64_store_general(writer, 1, logical_result_pointer_offset)) {
        return false;
    }
    const auto target_position = static_cast<std::uint32_t>(logical_hidden_result);
    if (!emit_x64_store_general(writer, logical_general[target_position], target_offset)) {
        return false;
    }
    for (std::uint32_t index = 0; index < call.argument_count; ++index) {
        const auto& value = call.arguments[index].value;
        const auto logical_position = index + 1 + static_cast<std::uint32_t>(logical_hidden_result);
        const auto indirect = x64_logical_indirect_argument(value);
        if (logical_position < 4) {
            const auto stored =
                indirect
                    ? emit_x64_store_general(writer, logical_general[logical_position], argument_pointer_offsets[index])
                : value.kind == FC_NATIVE_FLOAT_32 || value.kind == FC_NATIVE_FLOAT_64
                    ? emit_x64_store_simd(writer, static_cast<std::uint8_t>(logical_position),
                                          value.kind == FC_NATIVE_FLOAT_64, argument_offsets[index])
                    : emit_x64_store_general(writer, logical_general[logical_position], argument_offsets[index]);
            if (!stored) {
                return false;
            }
        } else {
            const auto source = frame_size + 40 + (logical_position - 4) * 8;
            if (indirect) {
                if (!emit_x64_copy_stack(writer, source, argument_pointer_offsets[index], 8)) {
                    return false;
                }
            } else if (!emit_x64_copy_stack(writer, source, argument_offsets[index], value.size)) {
                return false;
            }
        }
        if (indirect && !emit_x64_copy_pointer_to_stack(writer, argument_pointer_offsets[index],
                                                        argument_offsets[index], value.size)) {
            return false;
        }
    }

    // Preserve the logical caller's nonvolatile state even when the game's explicit convention uses those homes.
    for (std::size_t index = 0; index < nonvolatile_general.size(); ++index) {
        if (!emit_x64_store_general(writer, nonvolatile_general[index], general_offsets[index])) {
            return false;
        }
    }
    for (std::size_t index = 0; index < nonvolatile_simd.size(); ++index) {
        if (!emit_x64_store_simd_full(writer, nonvolatile_simd[index], simd_offsets[index])) {
            return false;
        }
    }

    // Stack homes are copied before register homes because the copier deliberately uses RAX as its scratch register.
    for (std::uint32_t index = 0; index < call.argument_count; ++index) {
        const auto& argument = call.arguments[index];
        if (argument.storage.kind == FC_NATIVE_STORAGE_STACK &&
            !emit_x64_copy_stack(writer, argument_offsets[index], 32 + argument.storage.stack_offset,
                                 argument.value.size)) {
            return false;
        }
    }
    if (call.result.kind == FC_NATIVE_RECORD) {
        const auto physical_pointer = x64_general_register(call.return_storage.register_id);
        if (call.return_storage.kind == FC_NATIVE_STORAGE_REGISTER) {
            if (!physical_pointer ||
                !(logical_hidden_result
                      ? emit_x64_load_general(writer, *physical_pointer, logical_result_pointer_offset)
                      : emit_x64_load_address(writer, *physical_pointer, result_offset))) {
                return false;
            }
        } else {
            if (logical_hidden_result) {
                if (!emit_x64_copy_stack(writer, logical_result_pointer_offset, 32 + call.return_storage.stack_offset,
                                         8)) {
                    return false;
                }
            } else if (!emit_x64_load_address(writer, 0, result_offset) ||
                       !emit_x64_store_general(writer, 0, 32 + call.return_storage.stack_offset)) {
                return false;
            }
        }
    }
    for (std::uint32_t index = 0; index < call.argument_count; ++index) {
        const auto& argument = call.arguments[index];
        if (argument.storage.kind != FC_NATIVE_STORAGE_REGISTER) {
            continue;
        }
        if (const auto general = x64_general_register(argument.storage.register_id)) {
            if (!emit_x64_load_general(writer, *general, argument_offsets[index])) {
                return false;
            }
        } else if (const auto simd = x64_simd_register(argument.storage.register_id)) {
            if (!emit_x64_load_simd(writer, *simd, argument.value.kind == FC_NATIVE_FLOAT_64,
                                    argument_offsets[index])) {
                return false;
            }
        } else {
            return false;
        }
    }
    if (!writer.append({0xff, 0x94, 0x24}) || !writer.append_value(target_offset)) {
        return false;
    }

    // Capture a possibly nonvolatile physical result before restoring the compiler ABI's incoming register values.
    if (call.result.kind != FC_NATIVE_VOID && call.result.kind != FC_NATIVE_RECORD) {
        if (const auto general = x64_general_register(call.return_storage.register_id)) {
            if (!emit_x64_store_general(writer, *general, result_offset)) {
                return false;
            }
        } else if (const auto simd = x64_simd_register(call.return_storage.register_id)) {
            if (!emit_x64_store_simd(writer, *simd, call.result.kind == FC_NATIVE_FLOAT_64, result_offset)) {
                return false;
            }
        } else {
            return false;
        }
    }
    for (std::size_t index = nonvolatile_simd.size(); index != 0; --index) {
        if (!emit_x64_load_simd_full(writer, nonvolatile_simd[index - 1], simd_offsets[index - 1])) {
            return false;
        }
    }
    for (std::size_t index = nonvolatile_general.size(); index != 0; --index) {
        if (!emit_x64_load_general(writer, nonvolatile_general[index - 1], general_offsets[index - 1])) {
            return false;
        }
    }
    if (call.result.kind == FC_NATIVE_RECORD) {
        if (!emit_x64_load_general(writer, 0, logical_hidden_result ? logical_result_pointer_offset : result_offset)) {
            return false;
        }
    } else if (call.result.kind == FC_NATIVE_FLOAT_32 || call.result.kind == FC_NATIVE_FLOAT_64) {
        if (!emit_x64_load_simd(writer, 0, call.result.kind == FC_NATIVE_FLOAT_64, result_offset)) {
            return false;
        }
    } else if (call.result.kind != FC_NATIVE_VOID && !emit_x64_load_general(writer, 0, result_offset)) {
        return false;
    }
    return emit_x64_stack_adjust(writer, false, frame_size) && writer.append({0xc3});
}

#else

// Maps reviewed x86 ABI homes to encoder register numbers; unsupported homes fail generation instead of degrading.
[[nodiscard]] constexpr std::optional<std::uint8_t> x86_general_register(FC_NativeRegister value) noexcept {
    switch (value) {
    case FC_REGISTER_EAX:
        return std::uint8_t{0};
    case FC_REGISTER_ECX:
        return std::uint8_t{1};
    case FC_REGISTER_EDX:
        return std::uint8_t{2};
    case FC_REGISTER_EBX:
        return std::uint8_t{3};
    case FC_REGISTER_EBP:
        return std::uint8_t{5};
    case FC_REGISTER_ESI:
        return std::uint8_t{6};
    case FC_REGISTER_EDI:
        return std::uint8_t{7};
    default:
        return std::nullopt;
    }
}

[[nodiscard]] constexpr std::optional<std::uint8_t> x86_simd_register(FC_NativeRegister value) noexcept {
    if (value < FC_REGISTER_XMM0 || value > FC_REGISTER_XMM7) {
        return std::nullopt;
    }
    return static_cast<std::uint8_t>(value - FC_REGISTER_XMM0);
}

[[nodiscard]] inline bool emit_x86_stack_adjust(HookEntryWriter& writer, bool subtract, std::uint32_t amount) noexcept {
    if (amount == 0) {
        return true;
    }
    if (amount <= std::numeric_limits<std::uint8_t>::max()) {
        return writer.append(
            {0x83, static_cast<std::uint8_t>(subtract ? 0xec : 0xc4), static_cast<std::uint8_t>(amount)});
    }
    return writer.append({0x81, static_cast<std::uint8_t>(subtract ? 0xec : 0xc4)}) && writer.append_value(amount);
}

[[nodiscard]] inline bool emit_x86_store_general(HookEntryWriter& writer, std::uint8_t source,
                                                 std::uint32_t displacement) noexcept {
    return writer.append({0x89, static_cast<std::uint8_t>(0x84 | ((source & 7) << 3)), 0x24}) &&
           writer.append_value(displacement);
}

[[nodiscard]] inline bool emit_x86_load_general(HookEntryWriter& writer, std::uint8_t destination,
                                                std::uint32_t displacement) noexcept {
    return writer.append({0x8b, static_cast<std::uint8_t>(0x84 | ((destination & 7) << 3)), 0x24}) &&
           writer.append_value(displacement);
}

[[nodiscard]] inline bool emit_x86_load_address(HookEntryWriter& writer, std::uint8_t destination,
                                                std::uint32_t displacement) noexcept {
    return writer.append({0x8d, static_cast<std::uint8_t>(0x84 | ((destination & 7) << 3)), 0x24}) &&
           writer.append_value(displacement);
}

[[nodiscard]] inline bool emit_x86_store_simd(HookEntryWriter& writer, std::uint8_t source, bool is_double,
                                              std::uint32_t displacement) noexcept {
    const auto modrm = static_cast<std::uint8_t>(0x84 | ((source & 7) << 3));
    return writer.append({static_cast<std::uint8_t>(is_double ? 0xf2 : 0xf3), 0x0f, 0x11, modrm, 0x24}) &&
           writer.append_value(displacement);
}

[[nodiscard]] inline bool emit_x86_load_simd(HookEntryWriter& writer, std::uint8_t destination, bool is_double,
                                             std::uint32_t displacement) noexcept {
    const auto modrm = static_cast<std::uint8_t>(0x84 | ((destination & 7) << 3));
    return writer.append({static_cast<std::uint8_t>(is_double ? 0xf2 : 0xf3), 0x0f, 0x10, modrm, 0x24}) &&
           writer.append_value(displacement);
}

[[nodiscard]] inline bool emit_x86_copy_stack(HookEntryWriter& writer, std::uint32_t source, std::uint32_t destination,
                                              std::uint32_t size) noexcept {
    // Moving 4, 2, then 1 byte copies the exact tail without reading beyond a small record's declared extent.
    std::uint32_t copied = 0;
    while (size - copied >= 4) {
        if (!writer.append({0x8b, 0x84, 0x24}) || !writer.append_value(source + copied) ||
            !writer.append({0x89, 0x84, 0x24}) || !writer.append_value(destination + copied)) {
            return false;
        }
        copied += 4;
    }
    if (size - copied >= 2) {
        if (!writer.append({0x66, 0x8b, 0x84, 0x24}) || !writer.append_value(source + copied) ||
            !writer.append({0x66, 0x89, 0x84, 0x24}) || !writer.append_value(destination + copied)) {
            return false;
        }
        copied += 2;
    }
    if (size != copied) {
        return writer.append({0x8a, 0x84, 0x24}) && writer.append_value(source + copied) &&
               writer.append({0x88, 0x84, 0x24}) && writer.append_value(destination + copied);
    }
    return true;
}

[[nodiscard]] constexpr bool x86_logical_hidden_result(const FC_NativeValue& result) noexcept {
    return result.kind == FC_NATIVE_RECORD && result.size > 8;
}

// Generates the x86 boundary invoked at the hook site, including declared cleanup ownership and conversion between ST0
// or SIMD floating results and the ordinary typed helper's compiler ABI.
[[nodiscard]] inline bool emit_x86_typed_entry(HookEntryWriter& writer, const FC_NativeCall& call,
                                               std::uintptr_t snapshot_slot, std::uintptr_t dispatcher) noexcept {
    const auto logical_hidden_result = x86_logical_hidden_result(call.result);
    std::vector<std::uint32_t> spill_offsets;
    spill_offsets.reserve(call.argument_count);
    std::uint32_t spill_size = 0;
    for (std::uint32_t index = 0; index < call.argument_count; ++index) {
        const auto& value = call.arguments[index].value;
        spill_size = hook_align_up(spill_size, std::max(1U, value.alignment));
        spill_offsets.push_back(spill_size);
        spill_size += hook_align_up(value.size, 4);
    }
    const auto return_pointer_offset = hook_align_up(spill_size, 4);
    if (call.result.kind == FC_NATIVE_RECORD) {
        spill_size = return_pointer_offset + 4;
    }
    const auto result_temp_offset = hook_align_up(spill_size, 4);
    spill_size = result_temp_offset + 8;
    if (!emit_x86_stack_adjust(writer, true, spill_size)) {
        return false;
    }

    // Spill physical register homes first so EAX can safely copy every stack-resident value afterward.
    for (std::uint32_t index = 0; index < call.argument_count; ++index) {
        const auto& argument = call.arguments[index];
        if (argument.storage.kind != FC_NATIVE_STORAGE_REGISTER) {
            continue;
        }
        if (const auto general = x86_general_register(argument.storage.register_id)) {
            if (!emit_x86_store_general(writer, *general, spill_offsets[index])) {
                return false;
            }
        } else if (const auto simd = x86_simd_register(argument.storage.register_id);
                   simd && (argument.value.kind == FC_NATIVE_FLOAT_32 || argument.value.kind == FC_NATIVE_FLOAT_64)) {
            if (!emit_x86_store_simd(writer, *simd, argument.value.kind == FC_NATIVE_FLOAT_64, spill_offsets[index])) {
                return false;
            }
        } else {
            return false;
        }
    }
    for (std::uint32_t index = 0; index < call.argument_count; ++index) {
        const auto& argument = call.arguments[index];
        if (argument.storage.kind == FC_NATIVE_STORAGE_STACK &&
            !emit_x86_copy_stack(writer, spill_size + 4 + argument.storage.stack_offset, spill_offsets[index],
                                 argument.value.size)) {
            return false;
        }
    }

    if (call.result.kind == FC_NATIVE_RECORD) {
        if (call.return_storage.kind == FC_NATIVE_STORAGE_REGISTER) {
            const auto source = x86_general_register(call.return_storage.register_id);
            if (!source || !emit_x86_store_general(writer, *source, return_pointer_offset)) {
                return false;
            }
        } else if (!emit_x86_copy_stack(writer, spill_size + 4 + call.return_storage.stack_offset,
                                        return_pointer_offset, 4)) {
            return false;
        }
    }

    // Cdecl helper arguments are pushed right-to-left; a result pointer hidden by the compiler becomes the final input.
    std::uint32_t pushed = 0;
    for (std::uint32_t reverse = call.argument_count; reverse != 0; --reverse) {
        const auto index = reverse - 1;
        const auto padded = hook_align_up(call.arguments[index].value.size, 4);
        for (std::uint32_t chunk = padded; chunk != 0; chunk -= 4) {
            if (!writer.append({0xff, 0xb4, 0x24}) || !writer.append_value(spill_offsets[index] + chunk - 4 + pushed)) {
                return false;
            }
            pushed += 4;
        }
    }
    if (!writer.append({0x68}) || !writer.append_value(static_cast<std::uint32_t>(snapshot_slot))) {
        return false;
    }
    pushed += 4;
    if (logical_hidden_result) {
        if (!writer.append({0xff, 0xb4, 0x24}) || !writer.append_value(return_pointer_offset + pushed)) {
            return false;
        }
        pushed += 4;
    }
    if (!writer.append({0xb8}) || !writer.append_value(static_cast<std::uint32_t>(dispatcher)) ||
        !writer.append({0xff, 0xd0}) || !emit_x86_stack_adjust(writer, false, pushed)) {
        return false;
    }

    if (call.result.kind == FC_NATIVE_RECORD && !logical_hidden_result) {
        if (!emit_x86_load_general(writer, 1, return_pointer_offset)) {
            return false;
        }
        if (call.result.size >= 4) {
            if (!writer.append({0x89, 0x01})) {
                return false;
            }
            if (call.result.size == 8 && !writer.append({0x89, 0x51, 0x04})) {
                return false;
            }
        } else if (call.result.size == 2) {
            if (!writer.append({0x66, 0x89, 0x01})) {
                return false;
            }
        } else if (!writer.append({0x88, 0x01})) {
            return false;
        }
    } else if (call.result.kind == FC_NATIVE_FLOAT_32 || call.result.kind == FC_NATIVE_FLOAT_64) {
        if (const auto destination = x86_simd_register(call.return_storage.register_id)) {
            const auto is_double = call.result.kind == FC_NATIVE_FLOAT_64;
            // The logical C++ helper returns floating values in ST0; materialize once to move them into a SIMD home.
            if (!writer.append({static_cast<std::uint8_t>(is_double ? 0xdd : 0xd9), 0x9c, 0x24}) ||
                !writer.append_value(result_temp_offset) ||
                !emit_x86_load_simd(writer, *destination, is_double, result_temp_offset)) {
                return false;
            }
        } else if (call.return_storage.register_id != FC_REGISTER_ST0) {
            return false;
        }
    } else if (call.result.kind != FC_NATIVE_VOID && call.return_storage.kind == FC_NATIVE_STORAGE_REGISTER &&
               call.return_storage.register_id != FC_REGISTER_EAX) {
        const auto destination = x86_general_register(call.return_storage.register_id);
        if (!destination || !writer.append({0x89, static_cast<std::uint8_t>(0xc0 | *destination)})) {
            return false;
        }
    }
    if (!emit_x86_stack_adjust(writer, false, spill_size)) {
        return false;
    }
    if (call.cleanup == FC_STACK_CLEANUP_CALLEE && call.stack_size != 0) {
        return writer.append({0xc2}) && writer.append_value(static_cast<std::uint16_t>(call.stack_size));
    }
    return writer.append({0xc3});
}

// Generates the cdecl-facing reverse adapter used when an explicit x86 call must invoke game code with arbitrary
// reviewed general/SIMD registers, normalized stack homes, and the declared cleanup owner.
[[nodiscard]] inline bool emit_x86_native_invoker(HookEntryWriter& writer, const FC_NativeCall& call) {
    const auto logical_hidden_result = x86_logical_hidden_result(call.result);
    constexpr std::array<std::uint8_t, 4> nonvolatile_general{3, 5, 6, 7};
    std::uint64_t cursor = call.stack_size;
    cursor = (cursor + 3) & ~std::uint64_t{3};
    const auto target_offset = static_cast<std::uint32_t>(cursor);
    cursor += 4;
    std::vector<std::uint32_t> argument_offsets;
    argument_offsets.reserve(call.argument_count);
    for (std::uint32_t index = 0; index < call.argument_count; ++index) {
        cursor = (cursor + 3) & ~std::uint64_t{3};
        argument_offsets.push_back(static_cast<std::uint32_t>(cursor));
        cursor += hook_align_up(std::max(4U, call.arguments[index].value.size), 4);
    }
    cursor = (cursor + std::max(4U, call.result.alignment) - 1) &
             ~static_cast<std::uint64_t>(std::max(4U, call.result.alignment) - 1);
    const auto result_offset = static_cast<std::uint32_t>(cursor);
    cursor += std::max(8U, call.result.size);
    cursor = (cursor + 3) & ~std::uint64_t{3};
    const auto logical_result_pointer_offset = static_cast<std::uint32_t>(cursor);
    cursor += 4;
    std::array<std::uint32_t, nonvolatile_general.size()> general_offsets{};
    for (auto& offset : general_offsets) {
        offset = static_cast<std::uint32_t>(cursor);
        cursor += 4;
    }
    if (cursor > std::numeric_limits<std::uint32_t>::max()) {
        return false;
    }
    const auto frame_size = static_cast<std::uint32_t>(cursor);
    if (!emit_x86_stack_adjust(writer, true, frame_size)) {
        return false;
    }

    // A return pointer hidden by the compiler precedes the explicit cdecl target and shifts every logical argument.
    std::uint32_t logical_offset = 4;
    if (logical_hidden_result) {
        if (!emit_x86_copy_stack(writer, frame_size + logical_offset, logical_result_pointer_offset, 4)) {
            return false;
        }
        logical_offset += 4;
    }
    if (!emit_x86_copy_stack(writer, frame_size + logical_offset, target_offset, 4)) {
        return false;
    }
    logical_offset += 4;
    for (std::uint32_t index = 0; index < call.argument_count; ++index) {
        const auto& value = call.arguments[index].value;
        if (!emit_x86_copy_stack(writer, frame_size + logical_offset, argument_offsets[index], value.size)) {
            return false;
        }
        logical_offset += hook_align_up(std::max(4U, value.size), 4);
    }
    for (std::size_t index = 0; index < nonvolatile_general.size(); ++index) {
        if (!emit_x86_store_general(writer, nonvolatile_general[index], general_offsets[index])) {
            return false;
        }
    }

    // Publish stack homes before loading register homes because all general-purpose copies use EAX as scratch.
    for (std::uint32_t index = 0; index < call.argument_count; ++index) {
        const auto& argument = call.arguments[index];
        if (argument.storage.kind == FC_NATIVE_STORAGE_STACK &&
            !emit_x86_copy_stack(writer, argument_offsets[index], argument.storage.stack_offset, argument.value.size)) {
            return false;
        }
    }
    if (call.result.kind == FC_NATIVE_RECORD) {
        if (call.return_storage.kind == FC_NATIVE_STORAGE_REGISTER) {
            const auto destination = x86_general_register(call.return_storage.register_id);
            if (!destination ||
                !(logical_hidden_result ? emit_x86_load_general(writer, *destination, logical_result_pointer_offset)
                                        : emit_x86_load_address(writer, *destination, result_offset))) {
                return false;
            }
        } else if (logical_hidden_result) {
            if (!emit_x86_copy_stack(writer, logical_result_pointer_offset, call.return_storage.stack_offset, 4)) {
                return false;
            }
        } else if (!emit_x86_load_address(writer, 0, result_offset) ||
                   !emit_x86_store_general(writer, 0, call.return_storage.stack_offset)) {
            return false;
        }
    }
    for (std::uint32_t index = 0; index < call.argument_count; ++index) {
        const auto& argument = call.arguments[index];
        if (argument.storage.kind != FC_NATIVE_STORAGE_REGISTER) {
            continue;
        }
        if (const auto general = x86_general_register(argument.storage.register_id)) {
            if (!emit_x86_load_general(writer, *general, argument_offsets[index])) {
                return false;
            }
        } else if (const auto simd = x86_simd_register(argument.storage.register_id)) {
            if (!emit_x86_load_simd(writer, *simd, argument.value.kind == FC_NATIVE_FLOAT_64,
                                    argument_offsets[index])) {
                return false;
            }
        } else {
            return false;
        }
    }
    if (!writer.append({0xff, 0x94, 0x24}) || !writer.append_value(target_offset)) {
        return false;
    }
    if (call.cleanup == FC_STACK_CLEANUP_CALLEE && !emit_x86_stack_adjust(writer, true, call.stack_size)) {
        return false;
    }

    bool result_in_st0{};
    if (call.result.kind != FC_NATIVE_VOID && call.result.kind != FC_NATIVE_RECORD) {
        if (const auto general = x86_general_register(call.return_storage.register_id)) {
            if (!emit_x86_store_general(writer, *general, result_offset)) {
                return false;
            }
        } else if (const auto simd = x86_simd_register(call.return_storage.register_id)) {
            if (!emit_x86_store_simd(writer, *simd, call.result.kind == FC_NATIVE_FLOAT_64, result_offset)) {
                return false;
            }
        } else if (call.return_storage.register_id == FC_REGISTER_ST0) {
            result_in_st0 = true;
        } else {
            return false;
        }
    }
    for (std::size_t index = nonvolatile_general.size(); index != 0; --index) {
        if (!emit_x86_load_general(writer, nonvolatile_general[index - 1], general_offsets[index - 1])) {
            return false;
        }
    }
    if (call.result.kind == FC_NATIVE_RECORD) {
        if (logical_hidden_result) {
            if (!emit_x86_load_general(writer, 0, logical_result_pointer_offset)) {
                return false;
            }
        } else {
            if (!emit_x86_load_general(writer, 0, result_offset) ||
                (call.result.size == 8 && !emit_x86_load_general(writer, 2, result_offset + 4))) {
                return false;
            }
        }
    } else if (call.result.kind == FC_NATIVE_FLOAT_32 || call.result.kind == FC_NATIVE_FLOAT_64) {
        if (!result_in_st0) {
            const auto opcode = static_cast<std::uint8_t>(call.result.kind == FC_NATIVE_FLOAT_64 ? 0xdd : 0xd9);
            if (!writer.append({opcode, 0x84, 0x24}) || !writer.append_value(result_offset)) {
                return false;
            }
        }
    } else if (call.result.kind != FC_NATIVE_VOID && !emit_x86_load_general(writer, 0, result_offset)) {
        return false;
    }
    return emit_x86_stack_adjust(writer, false, frame_size) && writer.append({0xc3});
}

#endif

// Owns one reverse adapter private to the SDK for the lifetime of the plugin module. Generation happens on a writable
// page, then the page is sealed executable before any callable or hook dispatcher can publish its address.
class ExecutableNativeInvoker final {
  public:
    ExecutableNativeInvoker() noexcept = default;
    ExecutableNativeInvoker(const ExecutableNativeInvoker&) = delete;
    ExecutableNativeInvoker& operator=(const ExecutableNativeInvoker&) = delete;
    ExecutableNativeInvoker(ExecutableNativeInvoker&& other) noexcept
        : address_(std::exchange(other.address_, nullptr)) {}
    ExecutableNativeInvoker& operator=(ExecutableNativeInvoker&&) = delete;
    ~ExecutableNativeInvoker() {
        if (address_ != nullptr) {
            VirtualFree(address_, 0, MEM_RELEASE);
        }
    }

    [[nodiscard]] std::uintptr_t address() const noexcept {
        return reinterpret_cast<std::uintptr_t>(address_);
    }

    template <class Call> [[nodiscard]] static ExecutableNativeInvoker build() {
        // Size the private page from normalized call complexity before allocating any writable executable candidate.
        auto native_call = native_call_storage<Call>();
        if (native_call.call.argument_count > (std::numeric_limits<std::size_t>::max() - 4096) / std::size_t{96}) {
            return {};
        }
        const auto capacity = 4096 + static_cast<std::size_t>(native_call.call.argument_count) * 96;
        auto* entry =
            static_cast<std::uint8_t*>(VirtualAlloc(nullptr, capacity, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
        if (entry == nullptr) {
            return {};
        }
        // Emit while writable, then make the completed page executable exactly once before publishing its address.
        std::memset(entry, 0xcc, capacity);
        HookEntryWriter writer{{entry, capacity}};
#if defined(_M_X64)
        const auto built = emit_x64_native_invoker(writer, native_call.call);
#else
        const auto built = emit_x86_native_invoker(writer, native_call.call);
#endif
        DWORD previous{};
        if (!built || !VirtualProtect(entry, capacity, PAGE_EXECUTE_READ, &previous) ||
            !FlushInstructionCache(GetCurrentProcess(), entry, capacity)) {
            VirtualFree(entry, 0, MEM_RELEASE);
            return {};
        }
        ExecutableNativeInvoker result;
        result.address_ = entry;
        return result;
    }

  private:
    void* address_{};
};

template <class Call> [[nodiscard]] std::uintptr_t native_invoker() noexcept {
    static_assert(is_explicit_native_call_v<Call>);
    try {
        static const auto entry = ExecutableNativeInvoker::build<Call>();
        return entry.address();
    } catch (...) {
        return 0;
    }
}

// Uses an ordinary function pointer only when the compiler ABI is authentic; explicit layouts always cross a thunk.
template <class Call, class... Args>
decltype(auto) invoke_native_original(std::uintptr_t address, Args... arguments) noexcept {
    if constexpr (NativeFunction<Call>) {
        return reinterpret_cast<Call>(address)(arguments...);
    } else {
        return NativeCallable<Call>{address}(arguments...);
    }
}

// The typed helper owns hot-path ordering and state; generated machine code only adapts the physical call boundary.
template <class Call, class Signature = typename FunctionTraits<Call>::signature> struct TypedHookDispatcher;

template <class Call, class Result, class... Args> struct TypedHookDispatcher<Call, Result(Args...)> {
    static Result FC_CALL dispatch(const std::uintptr_t* snapshot_slot, Args... arguments) noexcept {
        // Acquire one immutable participant set for the entire invocation so publication cannot split its ordering.
        const auto address =
            std::atomic_ref{*const_cast<std::uintptr_t*>(snapshot_slot)}.load(std::memory_order_acquire);
        const auto* snapshot = reinterpret_cast<const FC_HookSnapshot*>(address);
        if (snapshot == nullptr) {
            if constexpr (!std::is_void_v<Result>) {
                return Result{};
            } else {
                return;
            }
        }

        // Before observers run in deterministic order and receive disjoint slices of invocation-local state.
        alignas(16) std::array<std::byte, 1024> state{};
        const auto incoming = capture_ambient_errors();
        for (std::uint32_t index = 0; index < snapshot->observer_count; ++index) {
            const auto& observer = snapshot->observers[index];
            if (observer.before == 0) {
                continue;
            }
            restore_ambient_errors(incoming);
            auto* observer_state = observer.state_size == 0 ? nullptr : state.data() + observer.state_offset;
            reinterpret_cast<void(FC_CALL*)(void*, Args..., void*)>(observer.before)(observer.context, arguments...,
                                                                                     observer_state);
        }

        // The owner replaces the original when present; without one, the dispatcher invokes only the original.
        if constexpr (std::is_void_v<Result>) {
            restore_ambient_errors(incoming);
            if (snapshot->owner.callback != 0) {
                reinterpret_cast<void(FC_CALL*)(void*, Args...)>(snapshot->owner.callback)(snapshot->owner.context,
                                                                                           arguments...);
            } else {
                invoke_native_original<Call>(snapshot->original, arguments...);
            }
            // After observers unwind in reverse order while seeing the owner/original's ambient error state.
            const auto outgoing = capture_ambient_errors();
            for (auto index = snapshot->observer_count; index != 0; --index) {
                const auto& observer = snapshot->observers[index - 1];
                if (observer.after == 0) {
                    continue;
                }
                restore_ambient_errors(outgoing);
                const auto* observer_state = observer.state_size == 0 ? nullptr : state.data() + observer.state_offset;
                reinterpret_cast<void(FC_CALL*)(void*, Args..., const void*)>(observer.after)(
                    observer.context, arguments..., observer_state);
            }
            restore_ambient_errors(outgoing);
        } else {
            restore_ambient_errors(incoming);
            Result result = snapshot->owner.callback != 0
                                ? reinterpret_cast<Result(FC_CALL*)(void*, Args...)>(snapshot->owner.callback)(
                                      snapshot->owner.context, arguments...)
                                : invoke_native_original<Call>(snapshot->original, arguments...);
            // After observers see both the immutable arguments and completed result, then unwind in reverse order.
            const auto outgoing = capture_ambient_errors();
            for (auto index = snapshot->observer_count; index != 0; --index) {
                const auto& observer = snapshot->observers[index - 1];
                if (observer.after == 0) {
                    continue;
                }
                restore_ambient_errors(outgoing);
                const auto* observer_state = observer.state_size == 0 ? nullptr : state.data() + observer.state_offset;
                reinterpret_cast<void(FC_CALL*)(void*, Args..., Result, const void*)>(observer.after)(
                    observer.context, arguments..., result, observer_state);
            }
            restore_ambient_errors(outgoing);
            return result;
        }
    }
};

// Validates the callback-scoped framework packet and emits one bounded typed entry for its stable snapshot slot.
template <class Call>
FC_CallStatus FC_CALL build_typed_hook(const FC_HookBuildInput* input, const FC_ErrorSink* error) noexcept {
    constexpr auto required_size = offsetof(FC_HookBuildInput, snapshot_slot) + sizeof(input->snapshot_slot);
    if (input == nullptr || input->struct_size < required_size || input->entry == nullptr ||
        input->entry_size != kGeneratedHookEntrySize || input->snapshot_slot == nullptr) {
        set_error(error, "The hook builder received an invalid entry extent or snapshot slot", "Build hook dispatcher");
        return FC_CALL_FAILED;
    }
    // Initializing the full declared extent makes any short or partially written builder result impossible to expose.
    std::memset(input->entry, 0xcc, input->entry_size);
    try {
        auto native_call = native_call_storage<Call>();
        if constexpr (is_explicit_native_call_v<Call>) {
            if (native_invoker<Call>() == 0) {
                set_error(error, "The explicit native call adapter could not be sealed executable",
                          "Build hook dispatcher");
                return FC_CALL_FAILED;
            }
        }
        HookEntryWriter writer{{input->entry, input->entry_size}};
#if defined(_M_X64)
        const bool built =
            emit_x64_typed_entry(writer, native_call.call, reinterpret_cast<std::uintptr_t>(input->snapshot_slot),
                                 reinterpret_cast<std::uintptr_t>(&TypedHookDispatcher<Call>::dispatch));
#else
        const bool built =
            emit_x86_typed_entry(writer, native_call.call, reinterpret_cast<std::uintptr_t>(input->snapshot_slot),
                                 reinterpret_cast<std::uintptr_t>(&TypedHookDispatcher<Call>::dispatch));
#endif
        if (built) {
            return FC_CALL_OK;
        }
    } catch (...) {
        // The ABI is nonthrowing; allocation or generation failures become the builder's ordinary failed status.
    }
    set_error(error, "The normalized native call could not fit the generated hook entry", "Build hook dispatcher");
    return FC_CALL_FAILED;
}

template <class Call> [[nodiscard]] constexpr FC_HookBuilder typed_hook_builder() noexcept {
    return {.build = &build_typed_hook<Call>, .entry_size = kGeneratedHookEntrySize};
}

// Instruction dispatch operates on SafetyHook's layout-compatible context and never exposes a typed Original.
inline void FC_CALL dispatch_instruction_hook(const std::uintptr_t* snapshot_slot, FC_CpuContext* cpu) noexcept {
    // One acquired snapshot fixes participant order while every callback observes the same mutable CPU context.
    const auto address = std::atomic_ref{*const_cast<std::uintptr_t*>(snapshot_slot)}.load(std::memory_order_acquire);
    const auto* snapshot = reinterpret_cast<const FC_HookSnapshot*>(address);
    if (snapshot == nullptr) {
        return;
    }
    // Instruction observers use the incoming ambient state on both sides because the owner has no typed return path.
    alignas(16) std::array<std::byte, 1024> state{};
    const auto incoming = capture_ambient_errors();
    for (std::uint32_t index = 0; index < snapshot->observer_count; ++index) {
        const auto& observer = snapshot->observers[index];
        if (observer.before == 0) {
            continue;
        }
        restore_ambient_errors(incoming);
        auto* observer_state = observer.state_size == 0 ? nullptr : state.data() + observer.state_offset;
        reinterpret_cast<void(FC_CALL*)(void*, const FC_CpuContext*, void*)>(observer.before)(observer.context, cpu,
                                                                                              observer_state);
    }
    restore_ambient_errors(incoming);
    if (snapshot->owner.callback != 0) {
        reinterpret_cast<void(FC_CALL*)(void*, FC_CpuContext*)>(snapshot->owner.callback)(snapshot->owner.context, cpu);
    }
    for (auto index = snapshot->observer_count; index != 0; --index) {
        const auto& observer = snapshot->observers[index - 1];
        if (observer.after == 0) {
            continue;
        }
        restore_ambient_errors(incoming);
        const auto* observer_state = observer.state_size == 0 ? nullptr : state.data() + observer.state_offset;
        reinterpret_cast<void(FC_CALL*)(void*, const FC_CpuContext*, const void*)>(observer.after)(observer.context,
                                                                                                   cpu, observer_state);
    }
    restore_ambient_errors(incoming);
}

// Emits the small architecture-specific bridge from SafetyHook's context callback to the shared instruction helper.
inline FC_CallStatus FC_CALL build_instruction_hook(const FC_HookBuildInput* input,
                                                    const FC_ErrorSink* error) noexcept {
    constexpr auto required_size = offsetof(FC_HookBuildInput, snapshot_slot) + sizeof(input->snapshot_slot);
    if (input == nullptr || input->struct_size < required_size || input->entry == nullptr ||
        input->entry_size != kGeneratedHookEntrySize || input->snapshot_slot == nullptr) {
        set_error(error, "The instruction builder received an invalid entry extent or snapshot slot",
                  "Build instruction dispatcher");
        return FC_CALL_FAILED;
    }
    std::memset(input->entry, 0xcc, input->entry_size);
    HookEntryWriter writer{{input->entry, input->entry_size}};
#if defined(_M_X64)
    const auto built =
        emit_x64_stack_adjust(writer, true, 40) && writer.append({0x48, 0x89, 0xca}) && writer.append({0x48, 0xb9}) &&
        writer.append_value(reinterpret_cast<std::uintptr_t>(input->snapshot_slot)) && writer.append({0x48, 0xb8}) &&
        writer.append_value(reinterpret_cast<std::uintptr_t>(&dispatch_instruction_hook)) &&
        writer.append({0xff, 0xd0}) && emit_x64_stack_adjust(writer, false, 40) && writer.append({0xc3});
#else
    const auto built =
        writer.append({0xff, 0x74, 0x24, 0x04, 0x68}) &&
        writer.append_value(static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(input->snapshot_slot))) &&
        writer.append({0xb8}) &&
        writer.append_value(static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(&dispatch_instruction_hook))) &&
        writer.append({0xff, 0xd0, 0x83, 0xc4, 0x08, 0xc3});
#endif
    if (!built) {
        set_error(error, "The instruction dispatcher did not fit its declared entry extent",
                  "Build instruction dispatcher");
        return FC_CALL_FAILED;
    }
    return FC_CALL_OK;
}

[[nodiscard]] constexpr FC_HookBuilder instruction_hook_builder() noexcept {
    return {.build = &build_instruction_hook, .entry_size = kGeneratedHookEntrySize};
}

// Owner thunks establish this plugin-local frame after the cross-DLL dispatcher restores authentic incoming state.
struct OriginalInvocationFrame {
    const void* binding{};
    AmbientErrorState incoming{};
    OriginalInvocationFrame* previous{};
};

inline thread_local OriginalInvocationFrame* current_original_invocation = nullptr;

// Finds the nearest active owner frame for this binding so nested sites retain their own ambient error boundary.
[[nodiscard]] inline std::optional<AmbientErrorState> matching_original_errors(const void* binding) noexcept {
    for (auto* frame = current_original_invocation; frame != nullptr; frame = frame->previous) {
        if (frame->binding == binding) {
            return frame->incoming;
        }
    }
    return std::nullopt;
}

// Publishes the installed original target into the retainable author-facing Original handle.
template <class Call> void FC_CALL bind_original(void* context, std::uintptr_t original) {
    auto& binding = *static_cast<OriginalBinding<Call>*>(context);
    if (original == 0) {
        binding.address.store(0, std::memory_order_release);
        binding.invoker.store(0, std::memory_order_relaxed);
        return;
    }
    if constexpr (is_explicit_native_call_v<Call>) {
        const auto invoker = native_invoker<Call>();
        binding.invoker.store(invoker, std::memory_order_relaxed);
        binding.address.store(invoker == 0 ? 0 : original, std::memory_order_release);
    } else {
        binding.address.store(original, std::memory_order_release);
    }
}

// Owns a typed interface callback and copies the framework-provided bytes before invoking plugin code.
template <class Interface, class Callback> struct InterfaceBinding {
    Callback callback;

    static void FC_CALL connect(void* context, const void* value) {
        Interface copy{};
        std::memcpy(&copy, value, sizeof(copy));
        std::invoke(static_cast<InterfaceBinding*>(context)->callback, copy);
    }
};

} // namespace detail

// Retainable call-through handle: unbound during planning, bound before the Prepare callback, valid while installed.
template <class Call> class Original {
  public:
    [[nodiscard]] explicit operator bool() const noexcept {
        return binding_ != nullptr && binding_->address.load(std::memory_order_acquire) != 0;
    }

    template <class... Args> decltype(auto) operator()(Args&&... arguments) const noexcept {
        const auto address = binding_ == nullptr ? 0 : binding_->address.load(std::memory_order_acquire);
        assert(address != 0);
        // Matching owner calls restore the dispatcher's incoming state; calls elsewhere preserve their own boundary.
        const auto ambient =
            detail::matching_original_errors(binding_.get()).value_or(detail::capture_ambient_errors());
        detail::restore_ambient_errors(ambient);
        if constexpr (NativeFunction<Call>) {
            return reinterpret_cast<Call>(address)(std::forward<Args>(arguments)...);
        } else {
            const auto invoker = binding_->invoker.load(std::memory_order_relaxed);
            return detail::NativeCallable<Call>{address, invoker}(std::forward<Args>(arguments)...);
        }
    }

  private:
    explicit Original(std::shared_ptr<detail::OriginalBinding<Call>> binding) : binding_(std::move(binding)) {}
    std::shared_ptr<detail::OriginalBinding<Call>> binding_;
    friend class Plan;
};

namespace detail {

template <class T> struct ObserverTagTraits;

template <class Callback, bool IsBefore> struct ObserverTagTraits<ObserverTag<Callback, IsBefore>> {
    using callback = Callback;
    static constexpr bool is_before = IsBefore;
};

template <class T>
concept ObserverTagType = requires {
    typename ObserverTagTraits<std::remove_cvref_t<T>>::callback;
    { ObserverTagTraits<std::remove_cvref_t<T>>::is_before } -> std::convertible_to<bool>;
};

// Hook callback predicates reject throwing or signature-incompatible owners and observers at compile time.
template <class Callback, class Call, std::size_t... Indexes>
[[nodiscard]] consteval bool valid_owner_callback(std::index_sequence<Indexes...>) {
    using ResultType = CallResult<Call>;
    using Arguments = CallArguments<Call>;
    if constexpr (!std::is_nothrow_invocable_v<Callback&, Original<Call>,
                                               std::tuple_element_t<Indexes, Arguments>...>) {
        return false;
    } else {
        return std::same_as<
            std::invoke_result_t<Callback&, Original<Call>, std::tuple_element_t<Indexes, Arguments>...>, ResultType>;
    }
}

template <class Callback, class Call> [[nodiscard]] consteval bool valid_owner_callback() {
    return valid_owner_callback<Callback, Call>(std::make_index_sequence<std::tuple_size_v<CallArguments<Call>>>{});
}

template <class Callback, class Call, class State, std::size_t... Indexes>
[[nodiscard]] consteval bool valid_before_callback(std::index_sequence<Indexes...>) {
    using Arguments = CallArguments<Call>;
    if constexpr (std::is_void_v<State>) {
        return std::is_nothrow_invocable_r_v<void, Callback&, std::tuple_element_t<Indexes, Arguments>...>;
    } else {
        return std::is_nothrow_invocable_r_v<void, Callback&, std::tuple_element_t<Indexes, Arguments>..., State&>;
    }
}

template <class Callback, class Call, class State> [[nodiscard]] consteval bool valid_before_callback() {
    return valid_before_callback<Callback, Call, State>(
        std::make_index_sequence<std::tuple_size_v<CallArguments<Call>>>{});
}

template <class Callback, class Call, class State, std::size_t... Indexes>
[[nodiscard]] consteval bool valid_after_callback(std::index_sequence<Indexes...>) {
    using ResultType = CallResult<Call>;
    using Arguments = CallArguments<Call>;
    if constexpr (std::is_void_v<ResultType> && std::is_void_v<State>) {
        return std::is_nothrow_invocable_r_v<void, Callback&, std::tuple_element_t<Indexes, Arguments>...>;
    } else if constexpr (std::is_void_v<ResultType>) {
        return std::is_nothrow_invocable_r_v<void, Callback&, std::tuple_element_t<Indexes, Arguments>...,
                                             const State&>;
    } else if constexpr (std::is_void_v<State>) {
        return std::is_nothrow_invocable_r_v<void, Callback&, std::tuple_element_t<Indexes, Arguments>..., ResultType>;
    } else {
        return std::is_nothrow_invocable_r_v<void, Callback&, std::tuple_element_t<Indexes, Arguments>..., ResultType,
                                             const State&>;
    }
}

template <class Callback, class Call, class State> [[nodiscard]] consteval bool valid_after_callback() {
    return valid_after_callback<Callback, Call, State>(
        std::make_index_sequence<std::tuple_size_v<CallArguments<Call>>>{});
}

// One retained owner object keeps the author callback and its Original handle on the same installed lifetime.
template <class Call, class Callback> struct HookOwnerBinding {
    Callback callback;
    Original<Call> original;
    OriginalBinding<Call>* original_binding{};
};

template <class Call, class Binding, class Signature = typename FunctionTraits<Call>::signature> struct HookOwnerThunk;

template <class Call, class Binding, class Result, class... Args>
struct HookOwnerThunk<Call, Binding, Result(Args...)> {
    static Result FC_CALL invoke(void* context, Args... arguments) noexcept {
        auto& binding = *static_cast<Binding*>(context);
        OriginalInvocationFrame frame{binding.original_binding, capture_ambient_errors(), current_original_invocation};
        current_original_invocation = &frame;
        if constexpr (std::is_void_v<Result>) {
            std::invoke(binding.callback, binding.original, arguments...);
            current_original_invocation = frame.previous;
        } else {
            Result result = std::invoke(binding.callback, binding.original, arguments...);
            current_original_invocation = frame.previous;
            return result;
        }
    }
};

template <class Callback> struct SingleObserverBinding {
    Callback callback;
};

template <class Before, class After> struct PairedObserverBinding {
    Before before;
    After after;
};

// Observer thunks adapt retained author callables to the uniform callback and state pointer contracts used across DLLs.
template <class Call, class Binding, class State, class Signature = typename FunctionTraits<Call>::signature>
struct SingleBeforeThunk;

template <class Call, class Binding, class State, class Result, class... Args>
struct SingleBeforeThunk<Call, Binding, State, Result(Args...)> {
    static void FC_CALL invoke(void* context, Args... arguments, void* state) noexcept {
        auto& callback = static_cast<Binding*>(context)->callback;
        if constexpr (std::is_void_v<State>) {
            (void)state;
            std::invoke(callback, arguments...);
        } else {
            std::invoke(callback, arguments..., *static_cast<State*>(state));
        }
    }
};

template <class Call, class Binding, class State, class Signature = typename FunctionTraits<Call>::signature>
struct SingleAfterThunk;

template <class Call, class Binding, class State, class... Args>
struct SingleAfterThunk<Call, Binding, State, void(Args...)> {
    static void FC_CALL invoke(void* context, Args... arguments, const void* state) noexcept {
        auto& callback = static_cast<Binding*>(context)->callback;
        if constexpr (std::is_void_v<State>) {
            (void)state;
            std::invoke(callback, arguments...);
        } else {
            std::invoke(callback, arguments..., *static_cast<const State*>(state));
        }
    }
};

template <class Call, class Binding, class State, class Result, class... Args>
struct SingleAfterThunk<Call, Binding, State, Result(Args...)> {
    static_assert(!std::is_void_v<Result>);

    static void FC_CALL invoke(void* context, Args... arguments, Result result, const void* state) noexcept {
        auto& callback = static_cast<Binding*>(context)->callback;
        if constexpr (std::is_void_v<State>) {
            (void)state;
            std::invoke(callback, arguments..., result);
        } else {
            std::invoke(callback, arguments..., result, *static_cast<const State*>(state));
        }
    }
};

template <class Call, class Binding, class State, class Signature = typename FunctionTraits<Call>::signature>
struct PairedBeforeThunk;

template <class Call, class Binding, class State, class Result, class... Args>
struct PairedBeforeThunk<Call, Binding, State, Result(Args...)> {
    static void FC_CALL invoke(void* context, Args... arguments, void* state) noexcept {
        auto& callback = static_cast<Binding*>(context)->before;
        if constexpr (std::is_void_v<State>) {
            (void)state;
            std::invoke(callback, arguments...);
        } else {
            // The SDK owns the initialized lifetime of the author type hidden in framework byte storage.
            auto* value = std::construct_at(static_cast<State*>(state));
            std::invoke(callback, arguments..., *value);
        }
    }
};

template <class Call, class Binding, class State, class Signature = typename FunctionTraits<Call>::signature>
struct PairedAfterThunk;

template <class Call, class Binding, class State, class... Args>
struct PairedAfterThunk<Call, Binding, State, void(Args...)> {
    static void FC_CALL invoke(void* context, Args... arguments, const void* state) noexcept {
        auto& callback = static_cast<Binding*>(context)->after;
        if constexpr (std::is_void_v<State>) {
            (void)state;
            std::invoke(callback, arguments...);
        } else {
            auto* value = const_cast<State*>(static_cast<const State*>(state));
            std::invoke(callback, arguments..., std::as_const(*value));
            // Paired after is the final observer use, so even a trivial object's invocation lifetime ends here.
            std::destroy_at(value);
        }
    }
};

template <class Call, class Binding, class State, class Result, class... Args>
struct PairedAfterThunk<Call, Binding, State, Result(Args...)> {
    static_assert(!std::is_void_v<Result>);

    static void FC_CALL invoke(void* context, Args... arguments, Result result, const void* state) noexcept {
        auto& callback = static_cast<Binding*>(context)->after;
        if constexpr (std::is_void_v<State>) {
            (void)state;
            std::invoke(callback, arguments..., result);
        } else {
            auto* value = const_cast<State*>(static_cast<const State*>(state));
            std::invoke(callback, arguments..., result, std::as_const(*value));
            std::destroy_at(value);
        }
    }
};

template <class Callback> struct InstructionOwnerBinding {
    Callback callback;
};

template <class Binding> struct InstructionOwnerThunk {
    static void FC_CALL invoke(void* context, FC_CpuContext* cpu) noexcept {
        std::invoke(static_cast<Binding*>(context)->callback, *cpu);
    }
};

template <class Binding, class State> struct InstructionSingleBeforeThunk {
    static void FC_CALL invoke(void* context, const FC_CpuContext* cpu, void* state) noexcept {
        auto& callback = static_cast<Binding*>(context)->callback;
        if constexpr (std::is_void_v<State>) {
            (void)state;
            std::invoke(callback, *cpu);
        } else {
            std::invoke(callback, *cpu, *static_cast<State*>(state));
        }
    }
};

template <class Binding, class State> struct InstructionSingleAfterThunk {
    static void FC_CALL invoke(void* context, const FC_CpuContext* cpu, const void* state) noexcept {
        auto& callback = static_cast<Binding*>(context)->callback;
        if constexpr (std::is_void_v<State>) {
            (void)state;
            std::invoke(callback, *cpu);
        } else {
            std::invoke(callback, *cpu, *static_cast<const State*>(state));
        }
    }
};

template <class Binding, class State> struct InstructionPairedBeforeThunk {
    static void FC_CALL invoke(void* context, const FC_CpuContext* cpu, void* state) noexcept {
        auto& callback = static_cast<Binding*>(context)->before;
        if constexpr (std::is_void_v<State>) {
            (void)state;
            std::invoke(callback, *cpu);
        } else {
            auto* value = std::construct_at(static_cast<State*>(state));
            std::invoke(callback, *cpu, *value);
        }
    }
};

template <class Binding, class State> struct InstructionPairedAfterThunk {
    static void FC_CALL invoke(void* context, const FC_CpuContext* cpu, const void* state) noexcept {
        auto& callback = static_cast<Binding*>(context)->after;
        if constexpr (std::is_void_v<State>) {
            (void)state;
            std::invoke(callback, *cpu);
        } else {
            auto* value = const_cast<State*>(static_cast<const State*>(state));
            std::invoke(callback, *cpu, std::as_const(*value));
            std::destroy_at(value);
        }
    }
};

} // namespace detail

// Authoring surface that synchronously lowers one Plan callback's work into the framework-owned plan sink.
// The first rejected operation is sticky and makes subsequent submissions no-ops until the callback fails.
class Plan {
  public:
    [[nodiscard]] TargetInfo target() const noexcept {
        return target_;
    }
    [[nodiscard]] Logger logger() const noexcept {
        return logger_;
    }

    // Requirements validate locations and return addresses only for synchronously accepted native state.
    template <NativeData T, std::size_t Count>
        requires(Count > 0)
    const T* require(const DataLocation<T, Count>& location) {
        return reinterpret_cast<const T*>(
            submit_require(detail::PlanAccess::location(location), sizeof(T) * Count, alignof(T), false, nullptr));
    }

    template <NativeData T, std::size_t Count>
        requires(Count > 0)
    T* require_mutable(const DataLocation<T, Count>& location) {
        return reinterpret_cast<T*>(
            submit_require(detail::PlanAccess::location(location), sizeof(T) * Count, alignof(T), true, nullptr));
    }

    template <class Call> auto require(const FunctionLocation<Call>& location) {
        std::uintptr_t address = 0;
        if constexpr (detail::FunctionTraits<Call>::is_variadic) {
            address = submit_require(detail::PlanAccess::location(location), 1, 1, false, nullptr);
        } else {
            auto call = detail::native_call_storage<Call>();
            address = submit_require(detail::PlanAccess::location(location), 1, 1, false, &call.call);
        }
        if constexpr (NativeFunction<Call>) {
            return reinterpret_cast<Call>(address);
        } else {
            auto callable = detail::NativeCallable<Call>{address};
            if (address != 0 && !callable) {
                fail("The explicit native call adapter could not be sealed executable", "Resolve native function");
            }
            return callable;
        }
    }

    std::uintptr_t require(const CodeLocation& location, std::size_t size) {
        return submit_require(detail::PlanAccess::location(location), size, 1, false, nullptr);
    }

    std::uintptr_t require(const VtableLocation& table, std::size_t byte_size) {
        return submit_require(detail::PlanAccess::location(table), byte_size, 1, false, nullptr);
    }

    std::uintptr_t require_at(Rva rva, std::size_t size, Evidence evidence = {}) {
        return submit_require(detail::PlanAccess::compact(rva, FC_LOCATION_CODE, evidence), size, 1, false, nullptr);
    }

    // Writes and NOPs preserve evidence and replacement intent for later framework conflict and safety checks.
    template <class Location, class Replacement> void write(const Location& location, Replacement&& replacement) {
        submit_write(detail::PlanAccess::location(location), std::forward<Replacement>(replacement));
    }

    template <class Replacement> void write_at(Rva rva, Replacement&& replacement, Evidence evidence = {}) {
        FC_LocationKind kind = FC_LOCATION_DATA;
        if constexpr (detail::is_address_expression_v<Replacement>) {
            if constexpr (std::remove_cvref_t<Replacement>::kind == FC_WRITE_CALL ||
                          std::remove_cvref_t<Replacement>::kind == FC_WRITE_JUMP) {
                kind = FC_LOCATION_CODE;
            }
        }
        submit_write(detail::PlanAccess::compact(rva, kind, evidence), std::forward<Replacement>(replacement));
    }

    void nop(const CodeLocation& location, std::size_t size) {
        submit_nop(detail::PlanAccess::location(location), size);
    }

    void nop_at(Rva rva, std::size_t size, Evidence evidence = {}) {
        submit_nop(detail::PlanAccess::compact(rva, FC_LOCATION_CODE, evidence), size);
    }

    // Redirects return the currently decoded target while retaining any evidence for the secondary location.
    template <NativeFunction Function>
        requires detail::NativeCallType<Function>
    Function redirect_call(const CallLocation<Function>& site, Function replacement) {
        return reinterpret_cast<Function>(
            submit_redirect(detail::PlanAccess::location(site), FC_REDIRECT_CALL, detail::address_target(replacement)));
    }

    template <NativeFunction Function>
        requires detail::NativeCallType<Function>
    Function redirect_call(const CallLocation<Function>& site, const FunctionLocation<Function>& replacement) {
        consume_secondary_evidence(replacement);
        return reinterpret_cast<Function>(
            submit_redirect(detail::PlanAccess::location(site), FC_REDIRECT_CALL, detail::address_target(replacement)));
    }

    template <NativeFunction Function>
        requires detail::NativeCallType<Function>
    Function redirect_call_at(Rva rva, Function replacement, Evidence evidence = {}) {
        return reinterpret_cast<Function>(submit_redirect(detail::PlanAccess::compact(rva, FC_LOCATION_CODE, evidence),
                                                          FC_REDIRECT_CALL, detail::address_target(replacement)));
    }

    template <NativeFunction Function>
        requires detail::NativeCallType<Function>
    Function redirect_call_at(Rva rva, const FunctionLocation<Function>& replacement, Evidence evidence = {}) {
        consume_secondary_evidence(replacement);
        return reinterpret_cast<Function>(submit_redirect(detail::PlanAccess::compact(rva, FC_LOCATION_CODE, evidence),
                                                          FC_REDIRECT_CALL, detail::address_target(replacement)));
    }

    template <NativeFunction Function>
        requires detail::NativeCallType<Function>
    Function redirect_jump(const CodeLocation& site, Function replacement) {
        return reinterpret_cast<Function>(
            submit_redirect(detail::PlanAccess::location(site), FC_REDIRECT_JUMP, detail::address_target(replacement)));
    }

    template <NativeFunction Function>
        requires detail::NativeCallType<Function>
    Function redirect_jump(const CodeLocation& site, const FunctionLocation<Function>& replacement) {
        consume_secondary_evidence(replacement);
        return reinterpret_cast<Function>(
            submit_redirect(detail::PlanAccess::location(site), FC_REDIRECT_JUMP, detail::address_target(replacement)));
    }

    template <NativeFunction Function>
        requires detail::NativeCallType<Function>
    Function redirect_jump_at(Rva rva, Function replacement, Evidence evidence = {}) {
        return reinterpret_cast<Function>(submit_redirect(detail::PlanAccess::compact(rva, FC_LOCATION_CODE, evidence),
                                                          FC_REDIRECT_JUMP, detail::address_target(replacement)));
    }

    template <NativeFunction Function>
        requires detail::NativeCallType<Function>
    Function redirect_jump_at(Rva rva, const FunctionLocation<Function>& replacement, Evidence evidence = {}) {
        consume_secondary_evidence(replacement);
        return reinterpret_cast<Function>(submit_redirect(detail::PlanAccess::compact(rva, FC_LOCATION_CODE, evidence),
                                                          FC_REDIRECT_JUMP, detail::address_target(replacement)));
    }

    // Allocations remain symbolic during the Plan callback and become typed storage in the Prepare phase.
    template <NativeData T> DataHandle<T> allocate_data(std::size_t count = 1, std::string_view name = {}) {
        return allocate_data_impl<T>(count, {}, name);
    }

    template <NativeData T> DataHandle<T> allocate_data(std::span<const T> initial_values, std::string_view name = {}) {
        return allocate_data_impl<T>(initial_values.size(), std::as_bytes(initial_values), name);
    }

    // Hook owners receive a retainable Original binding; dispatcher construction is deferred to the runtime gate.
    template <class Call, class Callback>
        requires detail::NativeCallType<Call>
    Original<Call> hook(const FunctionLocation<Call>& location, Callback&& callback) {
        return submit_hook<Call>(detail::PlanAccess::location(location), FC_HOOK_FUNCTION_ENTRY,
                                 std::forward<Callback>(callback));
    }

    template <class Call, class Callback> Original<Call> hook(const CallLocation<Call>& location, Callback&& callback) {
        return submit_hook<Call>(detail::PlanAccess::location(location), FC_HOOK_DIRECT_CALL_SITE,
                                 std::forward<Callback>(callback));
    }

    template <class Callback> void hook(const CodeLocation& location, Callback&& callback) {
        submit_code_hook(detail::PlanAccess::location(location), std::forward<Callback>(callback));
    }

    // Observers compose optional before/after callbacks and bounded per-invocation state without owning the site.
    template <class Call, class Observer>
        requires detail::NativeCallType<Call>
    void observe(const FunctionLocation<Call>& location, Observer&& observer) {
        submit_observer<Call>(detail::PlanAccess::location(location), FC_HOOK_FUNCTION_ENTRY,
                              std::forward<Observer>(observer));
    }

    template <class Call, class Before, class After>
        requires detail::NativeCallType<Call>
    void observe(const FunctionLocation<Call>& location, Before&& before_callback, After&& after_callback) {
        submit_observer_pair<void, Call>(detail::PlanAccess::location(location), FC_HOOK_FUNCTION_ENTRY,
                                         std::forward<Before>(before_callback), std::forward<After>(after_callback));
    }

    template <class State, class Call, class Before, class After>
        requires detail::NativeCallType<Call>
    void observe(const FunctionLocation<Call>& location, Before&& before_callback, After&& after_callback) {
        submit_observer_pair<State, Call>(detail::PlanAccess::location(location), FC_HOOK_FUNCTION_ENTRY,
                                          std::forward<Before>(before_callback), std::forward<After>(after_callback));
    }

    template <class Call, class Observer> void observe(const CallLocation<Call>& location, Observer&& observer) {
        submit_observer<Call>(detail::PlanAccess::location(location), FC_HOOK_DIRECT_CALL_SITE,
                              std::forward<Observer>(observer));
    }

    template <class Call, class Before, class After>
    void observe(const CallLocation<Call>& location, Before&& before_callback, After&& after_callback) {
        submit_observer_pair<void, Call>(detail::PlanAccess::location(location), FC_HOOK_DIRECT_CALL_SITE,
                                         std::forward<Before>(before_callback), std::forward<After>(after_callback));
    }

    template <class State, class Call, class Before, class After>
    void observe(const CallLocation<Call>& location, Before&& before_callback, After&& after_callback) {
        submit_observer_pair<State, Call>(detail::PlanAccess::location(location), FC_HOOK_DIRECT_CALL_SITE,
                                          std::forward<Before>(before_callback), std::forward<After>(after_callback));
    }

    template <class Observer> void observe(const CodeLocation& location, Observer&& observer) {
        submit_code_observer(detail::PlanAccess::location(location), std::forward<Observer>(observer));
    }

    template <class Before, class After>
    void observe(const CodeLocation& location, Before&& before_callback, After&& after_callback) {
        submit_code_observer_pair<void>(detail::PlanAccess::location(location), std::forward<Before>(before_callback),
                                        std::forward<After>(after_callback));
    }

    template <class State, class Before, class After>
    void observe(const CodeLocation& location, Before&& before_callback, After&& after_callback) {
        submit_code_observer_pair<State>(detail::PlanAccess::location(location), std::forward<Before>(before_callback),
                                         std::forward<After>(after_callback));
    }

    template <class Call, class Callback>
        requires detail::NativeCallType<Call>
    Original<Call> hook_entry_at(Rva rva, Callback&& callback, Evidence evidence = {}) {
        FunctionLocation<Call> location{.rva = rva, .evidence = std::move(evidence)};
        return hook(location, std::forward<Callback>(callback));
    }

    template <class Call, class Callback>
    Original<Call> hook_call_at(Rva rva, Callback&& callback, Evidence evidence = {}) {
        CallLocation<Call> location{.rva = rva, .evidence = std::move(evidence)};
        return hook(location, std::forward<Callback>(callback));
    }

    template <class Callback> void hook_code_at(Rva rva, Callback&& callback, Evidence evidence = {}) {
        hook(CodeLocation{.rva = rva, .evidence = std::move(evidence)}, std::forward<Callback>(callback));
    }

    template <class Call, class Observer>
        requires detail::NativeCallType<Call>
    void observe_entry_at(Rva rva, Observer&& observer, Evidence evidence = {}) {
        observe(FunctionLocation<Call>{.rva = rva, .evidence = std::move(evidence)}, std::forward<Observer>(observer));
    }

    template <class Call, class Before, class After>
        requires detail::NativeCallType<Call>
    void observe_entry_at(Rva rva, Before&& before_callback, After&& after_callback, Evidence evidence = {}) {
        observe(FunctionLocation<Call>{.rva = rva, .evidence = std::move(evidence)},
                std::forward<Before>(before_callback), std::forward<After>(after_callback));
    }

    template <class Call, class State, class Before, class After>
        requires detail::NativeCallType<Call>
    void observe_entry_at(Rva rva, Before&& before_callback, After&& after_callback, Evidence evidence = {}) {
        observe<State>(FunctionLocation<Call>{.rva = rva, .evidence = std::move(evidence)},
                       std::forward<Before>(before_callback), std::forward<After>(after_callback));
    }

    template <class Call, class Observer> void observe_call_at(Rva rva, Observer&& observer, Evidence evidence = {}) {
        observe(CallLocation<Call>{.rva = rva, .evidence = std::move(evidence)}, std::forward<Observer>(observer));
    }

    template <class Call, class Before, class After>
    void observe_call_at(Rva rva, Before&& before_callback, After&& after_callback, Evidence evidence = {}) {
        observe(CallLocation<Call>{.rva = rva, .evidence = std::move(evidence)}, std::forward<Before>(before_callback),
                std::forward<After>(after_callback));
    }

    template <class Call, class State, class Before, class After>
    void observe_call_at(Rva rva, Before&& before_callback, After&& after_callback, Evidence evidence = {}) {
        observe<State>(CallLocation<Call>{.rva = rva, .evidence = std::move(evidence)},
                       std::forward<Before>(before_callback), std::forward<After>(after_callback));
    }

    template <class Observer> void observe_code_at(Rva rva, Observer&& observer, Evidence evidence = {}) {
        observe(CodeLocation{.rva = rva, .evidence = std::move(evidence)}, std::forward<Observer>(observer));
    }

    template <class Before, class After>
    void observe_code_at(Rva rva, Before&& before_callback, After&& after_callback, Evidence evidence = {}) {
        observe(CodeLocation{.rva = rva, .evidence = std::move(evidence)}, std::forward<Before>(before_callback),
                std::forward<After>(after_callback));
    }

    template <class State, class Before, class After>
    void observe_code_at(Rva rva, Before&& before_callback, After&& after_callback, Evidence evidence = {}) {
        observe<State>(CodeLocation{.rva = rva, .evidence = std::move(evidence)}, std::forward<Before>(before_callback),
                       std::forward<After>(after_callback));
    }

    // Interface bindings retain their callback until the framework connects the provider after its Activate callback.
    template <InterfaceContract Interface, class Callback>
    void bind(std::string_view provider_patch, Callback&& connect) {
        using Binding = detail::InterfaceBinding<Interface, std::decay_t<Callback>>;
        static_assert(std::is_nothrow_invocable_r_v<void, std::decay_t<Callback>&, Interface>,
                      "Plan::bind requires a nonthrowing void(Interface) callback");
        auto binding = std::make_shared<Binding>(Binding{.callback = std::forward<Callback>(connect)});
        FC_InterfaceBindingRequest request{.struct_size = sizeof(FC_InterfaceBindingRequest),
                                           .provider_patch = detail::string_view(provider_patch),
                                           .id = detail::string_view(std::string_view{Interface::id}),
                                           .size = sizeof(Interface),
                                           .context = binding.get(),
                                           .connect = &Binding::connect};
        if (!failed_ && (sink_ == nullptr || sink_->bind_interface == nullptr ||
                         sink_->bind_interface(sink_->context, &request) != FC_SUBMIT_ACCEPTED)) {
            fail("The interface binding was rejected", "Bind interface");
            return;
        }
        // The callback context outlives the Plan callback because the framework connects it after provider activation.
        bindings_->callbacks.push_back(std::move(binding));
    }

    template <InterfaceContract Interface, class Consumer>
    void bind(std::string_view provider_patch, Consumer& consumer, void (Consumer::*connect)(Interface) noexcept) {
        bind<Interface>(provider_patch, [&consumer, connect](Interface value) noexcept {
            (consumer.*connect)(std::move(value));
        });
    }

    void fail(std::string_view message, std::string_view operation = {}) {
        // The first failure is authoritative; later author calls remain safe no-ops and cannot obscure its cause.
        if (!failed_) {
            failed_ = true;
            error_.message = std::string{message};
            error_.operation = std::string{operation};
        }
    }

  private:
    Plan(const FC_HostApi* host, const FC_PlanContext* context, const FC_PlanSink* sink,
         detail::PatchRuntimeBindings* bindings) noexcept
        : target_(detail::target_info(context->target)), logger_(detail::ContextFactory::logger(host, context->report)),
          sink_(sink), bindings_(bindings) {}

    // Private submitters translate overload intent into one ABI request and preserve the patch plan's first rejection.
    std::uintptr_t submit_require(FC_LocationView location, std::size_t size, std::size_t alignment, bool writable,
                                  const FC_NativeCall* native_call) {
        if (failed_ || size == 0) {
            if (size == 0) {
                fail("A native requirement cannot be empty", "Require native location");
            }
            return 0;
        }
        const FC_RequireRequest request{.struct_size = sizeof(FC_RequireRequest),
                                        .location = location,
                                        .size = size,
                                        .alignment = static_cast<std::uint32_t>(alignment),
                                        .writable = writable ? FC_TRUE : FC_FALSE,
                                        .native_call = native_call};
        std::uintptr_t address = 0;
        if (sink_ == nullptr || sink_->require == nullptr ||
            sink_->require(sink_->context, &request, &address) != FC_SUBMIT_ACCEPTED) {
            fail("The native requirement was rejected", "Require native location");
            return 0;
        }
        return address;
    }

    template <class Replacement> void submit_write(FC_LocationView location, Replacement&& replacement) {
        if (failed_) {
            return;
        }
        // The replacement's type selects exactly one representation: bytes, a pointer, or an encoded address.
        FC_WriteRequest request{.struct_size = sizeof(FC_WriteRequest), .location = location};
        if constexpr (detail::is_address_expression_v<Replacement>) {
            request.kind = std::remove_cvref_t<Replacement>::kind;
            request.target = detail::address_target(replacement.target);
        } else if constexpr (NativeFunction<std::remove_cvref_t<Replacement>>) {
            request.kind = FC_WRITE_POINTER;
            request.target = detail::address_target(replacement);
        } else if constexpr (detail::ImageLocation<std::remove_cvref_t<Replacement>> ||
                             std::same_as<std::remove_cvref_t<Replacement>, DataAddress>) {
            if constexpr (detail::ImageLocation<std::remove_cvref_t<Replacement>>) {
                consume_secondary_evidence(replacement);
            }
            request.kind = FC_WRITE_POINTER;
            request.target = detail::address_target(replacement);
        } else {
            const auto bytes = detail::replacement_bytes(replacement);
            request.kind = FC_WRITE_BYTES;
            request.bytes = detail::byte_view(bytes);
        }
        if (sink_ == nullptr || sink_->write == nullptr ||
            sink_->write(sink_->context, &request) != FC_SUBMIT_ACCEPTED) {
            fail("The native write was rejected", "Write native state");
        }
    }

    void submit_nop(FC_LocationView location, std::size_t size) {
        if (failed_) {
            return;
        }
        if (size == 0) {
            fail("A NOP range cannot be empty", "Plan NOP range");
            return;
        }
        const FC_NopRequest request{.struct_size = sizeof(FC_NopRequest), .location = location, .size = size};
        if (sink_ == nullptr || sink_->nop == nullptr || sink_->nop(sink_->context, &request) != FC_SUBMIT_ACCEPTED) {
            fail("The NOP range was rejected", "Plan NOP range");
        }
    }

    std::uintptr_t submit_redirect(FC_LocationView location, FC_RedirectKind kind, FC_AddressTarget target) {
        if (failed_) {
            return 0;
        }
        const FC_RedirectRequest request{
            .struct_size = sizeof(FC_RedirectRequest), .location = location, .kind = kind, .target = target};
        std::uintptr_t original = 0;
        if (sink_ == nullptr || sink_->redirect == nullptr ||
            sink_->redirect(sink_->context, &request, &original) != FC_SUBMIT_ACCEPTED) {
            fail("The branch redirect was rejected", "Redirect native branch");
            return 0;
        }
        return original;
    }

    template <NativeData T>
    DataHandle<T> allocate_data_impl(std::size_t count, std::span<const std::byte> initial_values,
                                     std::string_view name) {
        if (failed_ || count == 0 || count > std::numeric_limits<std::size_t>::max() / sizeof(T)) {
            fail("A native allocation must contain at least one element and cannot overflow", "Allocate native data");
            return {this, FC_INVALID_DATA_HANDLE, 0};
        }
        const auto byte_size = count * sizeof(T);
        const FC_DataAllocationRequest request{.struct_size = sizeof(FC_DataAllocationRequest),
                                               .byte_size = byte_size,
                                               .alignment = alignof(T),
                                               .initial_bytes = detail::byte_view(initial_values),
                                               .name = detail::string_view(name)};
        FC_DataHandle handle = FC_INVALID_DATA_HANDLE;
        if (sink_ == nullptr || sink_->allocate_data == nullptr ||
            sink_->allocate_data(sink_->context, &request, &handle) != FC_SUBMIT_ACCEPTED) {
            fail("The native allocation was rejected", "Allocate native data");
            return {this, FC_INVALID_DATA_HANDLE, 0};
        }
        // The Prepare adapter uses this metadata to validate the extent and begin T's implicit lifetime once.
        bindings_->allocations.push_back({.handle = handle,
                                          .count = count,
                                          .element_size = sizeof(T),
                                          .alignment = alignof(T),
                                          .start_lifetime = &detail::start_allocation_lifetime<T>});
        return {this, handle, count};
    }

    template <class Location> void consume_secondary_evidence(const Location& location) {
        // A referenced replacement location carries independent evidence that must become its own requirement.
        const auto native = detail::PlanAccess::evidence(location.evidence);
        if (native.kind == FC_EVIDENCE_NONE) {
            return;
        }
        if constexpr (detail::IsFunctionLocation<std::remove_cvref_t<Location>>::value) {
            using Call = typename std::remove_cvref_t<Location>::call_type;
            auto call = detail::native_call_storage<Call>();
            (void)submit_require(detail::PlanAccess::location(location), 1, 1, false, &call.call);
        } else {
            (void)submit_require(detail::PlanAccess::location(location), 1, 1, false, nullptr);
        }
    }

    template <class Call, class Callback>
    Original<Call> submit_hook(FC_LocationView location, FC_HookKind kind, Callback&& callback) {
        static_assert(detail::valid_owner_callback<std::decay_t<Callback>, Call>(),
                      "A typed hook owner must be a nonthrowing Result(Original<Call>, Args...) callback");
        auto original = std::make_shared<detail::OriginalBinding<Call>>();
        Original<Call> original_handle{original};
        if constexpr (detail::is_explicit_native_call_v<Call>) {
            // A late owner may never run its builder, so the Plan callback seals the reverse Original adapter itself.
            if (detail::native_invoker<Call>() == 0) {
                fail("The explicit Original adapter could not be sealed executable", "Plan shared hook");
                return original_handle;
            }
        }
        using Binding = detail::HookOwnerBinding<Call, std::decay_t<Callback>>;
        auto stored =
            std::make_shared<Binding>(Binding{std::forward<Callback>(callback), original_handle, original.get()});
        auto native_call = detail::native_call_storage<Call>();
        const FC_HookRequest request{
            .struct_size = sizeof(FC_HookRequest),
            .location = location,
            .kind = kind,
            .native_call = &native_call.call,
            .builder = detail::typed_hook_builder<Call>(),
            .context = stored.get(),
            .callback = reinterpret_cast<std::uintptr_t>(&detail::HookOwnerThunk<Call, Binding>::invoke),
            .original_context = original.get(),
            .bind_original = &detail::bind_original<Call>};
        if (!failed_ && (sink_ == nullptr || sink_->hook == nullptr ||
                         sink_->hook(sink_->context, &request) != FC_SUBMIT_ACCEPTED)) {
            fail("The hook owner was rejected", "Plan shared hook");
        } else {
            // The retained binding owns both callback and Original state after the callback-scoped request is copied.
            bindings_->callbacks.push_back(std::move(stored));
        }
        return original_handle;
    }

    template <class Callback> void submit_code_hook(FC_LocationView location, Callback&& callback) {
        static_assert(std::is_nothrow_invocable_r_v<void, std::decay_t<Callback>&, CpuContext&>,
                      "An instruction hook owner must be a nonthrowing void(CpuContext&) callback");
        // The retained binding owns author state while the request exposes only a stable generated C thunk.
        using Binding = detail::InstructionOwnerBinding<std::decay_t<Callback>>;
        auto stored = std::make_shared<Binding>(Binding{std::forward<Callback>(callback)});
        const FC_HookRequest request{
            .struct_size = sizeof(FC_HookRequest),
            .location = location,
            .kind = FC_HOOK_INSTRUCTION,
            .builder = detail::instruction_hook_builder(),
            .context = stored.get(),
            .callback = reinterpret_cast<std::uintptr_t>(&detail::InstructionOwnerThunk<Binding>::invoke)};
        if (!failed_ && (sink_ == nullptr || sink_->hook == nullptr ||
                         sink_->hook(sink_->context, &request) != FC_SUBMIT_ACCEPTED)) {
            fail("The instruction hook was rejected", "Plan instruction hook");
        } else {
            bindings_->callbacks.push_back(std::move(stored));
        }
    }

    template <class Call, bool Instruction = false, class Observer>
    void submit_observer(FC_LocationView location, FC_HookKind kind, Observer&& observer) {
        // Reject a mismatched before/after signature at composition time, before any callback state is retained.
        using Stored = std::decay_t<Observer>;
        static_assert(detail::ObserverTagType<Stored>,
                      "A hook observer must be wrapped in fc::before(...) or fc::after(...)");
        if constexpr (detail::ObserverTagType<Stored> && !Instruction) {
            using Callback = typename detail::ObserverTagTraits<Stored>::callback;
            if constexpr (detail::ObserverTagTraits<Stored>::is_before) {
                static_assert(detail::valid_before_callback<Callback, Call, void>(),
                              "A before observer must be a nonthrowing void(Args...) callback");
            } else {
                static_assert(detail::valid_after_callback<Callback, Call, void>(),
                              "An after observer must be a nonthrowing void(Args..., Result) callback");
            }
        }
        // One retained binding backs exactly one side of either the typed call or instruction observer boundary.
        Stored tag{std::forward<Observer>(observer)};
        using Callback = typename detail::ObserverTagTraits<Stored>::callback;
        using Binding = detail::SingleObserverBinding<Callback>;
        auto stored = std::make_shared<Binding>(Binding{std::move(tag.callback)});
        auto native_call = detail::native_call_storage<Call>();
        constexpr bool is_before = Stored::before;
        // Select the physical entry builder and the one non-null directional thunk from the compile-time shape.
        const auto builder = [] {
            if constexpr (Instruction) {
                return detail::instruction_hook_builder();
            } else {
                return detail::typed_hook_builder<Call>();
            }
        }();
        const auto before_thunk = [] {
            if constexpr (!is_before) {
                return std::uintptr_t{0};
            } else if constexpr (Instruction) {
                return reinterpret_cast<std::uintptr_t>(&detail::InstructionSingleBeforeThunk<Binding, void>::invoke);
            } else {
                return reinterpret_cast<std::uintptr_t>(&detail::SingleBeforeThunk<Call, Binding, void>::invoke);
            }
        }();
        const auto after_thunk = [] {
            if constexpr (is_before) {
                return std::uintptr_t{0};
            } else if constexpr (Instruction) {
                return reinterpret_cast<std::uintptr_t>(&detail::InstructionSingleAfterThunk<Binding, void>::invoke);
            } else {
                return reinterpret_cast<std::uintptr_t>(&detail::SingleAfterThunk<Call, Binding, void>::invoke);
            }
        }();
        // The framework copies this request synchronously; only the stored binding survives the Plan callback.
        const FC_ObserverRequest request{.struct_size = sizeof(FC_ObserverRequest),
                                         .location = location,
                                         .kind = kind,
                                         .native_call = Instruction ? nullptr : &native_call.call,
                                         .builder = builder,
                                         .context = stored.get(),
                                         .before = before_thunk,
                                         .after = after_thunk};
        if (!failed_ && (sink_ == nullptr || sink_->observe == nullptr ||
                         sink_->observe(sink_->context, &request) != FC_SUBMIT_ACCEPTED)) {
            fail("The hook observer was rejected", "Plan hook observer");
        } else {
            bindings_->callbacks.push_back(std::move(stored));
        }
    }

    template <class State, class Call, bool Instruction = false, class Before, class After>
    void submit_observer_pair(FC_LocationView location, FC_HookKind kind, Before&& before_callback,
                              After&& after_callback) {
        using BeforeTag = std::decay_t<Before>;
        using AfterTag = std::decay_t<After>;
        static_assert(detail::ObserverTagType<BeforeTag> && detail::ObserverTagType<AfterTag>,
                      "A paired observer requires fc::before(...) and fc::after(...) callbacks");
        static_assert(detail::ObserverTagTraits<BeforeTag>::is_before &&
                          !detail::ObserverTagTraits<AfterTag>::is_before,
                      "A paired observer requires its before callback first and after callback second");
        if constexpr (detail::ObserverTagType<BeforeTag> && detail::ObserverTagType<AfterTag> && !Instruction) {
            using BeforeCallback = typename detail::ObserverTagTraits<BeforeTag>::callback;
            using AfterCallback = typename detail::ObserverTagTraits<AfterTag>::callback;
            static_assert(detail::valid_before_callback<BeforeCallback, Call, State>(),
                          "A paired before observer has the wrong signature or can throw");
            static_assert(detail::valid_after_callback<AfterCallback, Call, State>(),
                          "A paired after observer has the wrong signature or can throw");
        }
        // Shared per-invocation state must be trivial because the generated dispatcher owns only raw bounded storage.
        if constexpr (!std::is_void_v<State>) {
            static_assert(std::is_trivial_v<State>, "A paired observer state must be trivial");
            static_assert(alignof(State) <= 16, "A paired observer state cannot require more than 16-byte alignment");
        }
        // Both author callbacks share one retained binding so their invocation-local state cannot outlive the pair.
        BeforeTag before_tag{std::forward<Before>(before_callback)};
        AfterTag after_tag{std::forward<After>(after_callback)};
        using BeforeCallback = typename detail::ObserverTagTraits<BeforeTag>::callback;
        using AfterCallback = typename detail::ObserverTagTraits<AfterTag>::callback;
        using Binding = detail::PairedObserverBinding<BeforeCallback, AfterCallback>;
        auto stored = std::make_shared<Binding>(Binding{std::move(before_tag.callback), std::move(after_tag.callback)});
        auto native_call = detail::native_call_storage<Call>();
        constexpr std::uint32_t state_size = [] {
            if constexpr (std::is_void_v<State>) {
                return std::uint32_t{0};
            } else {
                return static_cast<std::uint32_t>(sizeof(State));
            }
        }();
        constexpr std::uint32_t state_alignment = [] {
            if constexpr (std::is_void_v<State>) {
                return std::uint32_t{0};
            } else {
                return static_cast<std::uint32_t>(alignof(State));
            }
        }();
        // Compile-time instruction selection keeps the public overloads on the same native request representation.
        const auto builder = [] {
            if constexpr (Instruction) {
                return detail::instruction_hook_builder();
            } else {
                return detail::typed_hook_builder<Call>();
            }
        }();
        const auto before_thunk = [] {
            if constexpr (Instruction) {
                return reinterpret_cast<std::uintptr_t>(&detail::InstructionPairedBeforeThunk<Binding, State>::invoke);
            } else {
                return reinterpret_cast<std::uintptr_t>(&detail::PairedBeforeThunk<Call, Binding, State>::invoke);
            }
        }();
        const auto after_thunk = [] {
            if constexpr (Instruction) {
                return reinterpret_cast<std::uintptr_t>(&detail::InstructionPairedAfterThunk<Binding, State>::invoke);
            } else {
                return reinterpret_cast<std::uintptr_t>(&detail::PairedAfterThunk<Call, Binding, State>::invoke);
            }
        }();
        // Publish the paired callbacks and their one shared state extent as a single indivisible observer request.
        const FC_ObserverRequest request{.struct_size = sizeof(FC_ObserverRequest),
                                         .location = location,
                                         .kind = kind,
                                         .native_call = Instruction ? nullptr : &native_call.call,
                                         .builder = builder,
                                         .context = stored.get(),
                                         .before = before_thunk,
                                         .after = after_thunk,
                                         .state_size = state_size,
                                         .state_alignment = state_alignment};
        if (!failed_ && (sink_ == nullptr || sink_->observe == nullptr ||
                         sink_->observe(sink_->context, &request) != FC_SUBMIT_ACCEPTED)) {
            fail("The paired hook observer was rejected", "Plan hook observer");
        } else {
            bindings_->callbacks.push_back(std::move(stored));
        }
    }

    template <class Observer> void submit_code_observer(FC_LocationView location, Observer&& observer) {
        using Stored = std::decay_t<Observer>;
        static_assert(detail::ObserverTagType<Stored>,
                      "An instruction observer must be wrapped in fc::before(...) or fc::after(...)");
        using Callback = typename detail::ObserverTagTraits<Stored>::callback;
        static_assert(std::is_nothrow_invocable_r_v<void, Callback&, const CpuContext&>,
                      "An instruction observer must be a nonthrowing void(const CpuContext&) callback");
        submit_observer<void (*)() noexcept, true>(location, FC_HOOK_INSTRUCTION, std::forward<Observer>(observer));
    }

    template <class State, class Before, class After>
    void submit_code_observer_pair(FC_LocationView location, Before&& before_callback, After&& after_callback) {
        using BeforeTag = std::decay_t<Before>;
        using AfterTag = std::decay_t<After>;
        static_assert(detail::ObserverTagType<BeforeTag> && detail::ObserverTagType<AfterTag>,
                      "A paired instruction observer requires fc::before(...) and fc::after(...) callbacks");
        static_assert(detail::ObserverTagTraits<BeforeTag>::is_before &&
                          !detail::ObserverTagTraits<AfterTag>::is_before,
                      "A paired instruction observer requires its before callback first and after callback second");
        using BeforeCallback = typename detail::ObserverTagTraits<BeforeTag>::callback;
        using AfterCallback = typename detail::ObserverTagTraits<AfterTag>::callback;
        if constexpr (std::is_void_v<State>) {
            static_assert(std::is_nothrow_invocable_r_v<void, BeforeCallback&, const CpuContext&> &&
                              std::is_nothrow_invocable_r_v<void, AfterCallback&, const CpuContext&>,
                          "Instruction observer callbacks must be nonthrowing void(const CpuContext&) callables");
        } else {
            static_assert(std::is_nothrow_invocable_r_v<void, BeforeCallback&, const CpuContext&, State&> &&
                              std::is_nothrow_invocable_r_v<void, AfterCallback&, const CpuContext&, const State&>,
                          "Paired instruction observer callbacks have the wrong state signature or can throw");
        }
        submit_observer_pair<State, void (*)() noexcept, true>(
            location, FC_HOOK_INSTRUCTION, std::forward<Before>(before_callback), std::forward<After>(after_callback));
    }

    TargetInfo target_{};
    Logger logger_;
    const FC_PlanSink* sink_ = nullptr;
    detail::PatchRuntimeBindings* bindings_ = nullptr;
    bool failed_ = false;
    Error error_;

    friend struct detail::PluginAccess;
    template <class Handler> friend class detail::HandlerAdapter;
    template <class Handler>
    friend FC_CallStatus detail::invoke_plan(Handler& handler, const FC_HostApi* host,
                                             const FC_PlanContext* plan_context, const FC_PlanSink* sink,
                                             detail::PatchRuntimeBindings& bindings, const FC_ErrorSink* error);
};

template <NativeData T> DataAddress DataHandle<T>::base() const noexcept {
    return {owner_, handle_, 0, false};
}

template <NativeData T> DataAddress DataHandle<T>::element(std::size_t index) const noexcept {
    // Invalid symbolic derivations fail their patch plan instead of exposing a wrapped or foreign address.
    if (owner_ == nullptr || index >= count_) {
        if (owner_ != nullptr) {
            owner_->fail("A symbolic data element is outside its allocation", "Derive data address");
        }
        return {owner_, FC_INVALID_DATA_HANDLE, 0, false};
    }
    return {owner_, handle_, index * sizeof(T), false};
}

template <NativeData T> DataAddress DataHandle<T>::byte_offset(std::size_t offset) const noexcept {
    if (owner_ == nullptr || offset >= count_ * sizeof(T)) {
        if (owner_ != nullptr) {
            owner_->fail("A symbolic byte offset is outside its allocation", "Derive data address");
        }
        return {owner_, FC_INVALID_DATA_HANDLE, 0, false};
    }
    return {owner_, handle_, offset, false};
}

template <NativeData T> DataAddress DataHandle<T>::end() const noexcept {
    return {owner_, handle_, count_ * sizeof(T), true};
}

namespace detail {

template <class Handler>
FC_CallStatus invoke_plan(Handler& handler, const FC_HostApi* host, const FC_PlanContext* plan_context,
                          const FC_PlanSink* sink, PatchRuntimeBindings& bindings, const FC_ErrorSink* error) {
    // The Plan callback contains author exceptions and converts sticky helper failures to an ABI failure.
    try {
        Plan author_plan{host, plan_context, sink, &bindings};
        if constexpr (HasPlan<Handler>) {
            handler.plan(author_plan);
        }
        if (author_plan.failed_) {
            set_error(error, author_plan.error_.message, author_plan.error_.operation);
            return FC_CALL_FAILED;
        }
        return FC_CALL_OK;
    } catch (const std::exception& exception) {
        set_error(error, exception.what(), "Build patch plan");
    } catch (...) {
        set_error(error, "The patch's Plan callback threw an unknown exception", "Build patch plan");
    }
    return FC_CALL_FAILED;
}

} // namespace detail

} // namespace fc
