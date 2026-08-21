#include <FusionCutter/SDK.hpp>

#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

// Typed settings cover numeric constraints, Boolean conversion, owned strings, and lowering in declaration order.
struct PatchSettings {
    std::uint16_t limit;
    bool enabled;
    std::string label;
};

fc::SettingsSchema<PatchSettings> patch_settings() {
    return fc::settings<PatchSettings>(
        fc::value("Limit", &PatchSettings::limit, std::uint16_t{64}).range(std::uint16_t{1}, std::uint16_t{512}),
        fc::value("Enabled", &PatchSettings::enabled, true),
        fc::value("Label", &PatchSettings::label, std::string{"contract"}).max_length(32));
}

// A small author enum exercises choice lowering and reconstruction without adding behavior to the fixture.
enum class ContractPolicy {
    Observe,
    Enforce,
};

// This aggregate receives every public way to compose settings through one generated Create callback.
struct CompositionSettings {
    bool enabled{};
    std::int32_t limit{};
    double ratio{};
    std::string label;
    ContractPolicy policy{};
    std::string initializer_text;
    std::uint32_t range_value{};
    fc::StringMap bindings;
    fc::StringMap aliases;
};

// The handler copies one completed object here so the test can inspect it after the Create callback returns.
std::optional<CompositionSettings> constructed_composition;

// Validation runs after every flat value is assigned, so normalization is visible to the handler constructor.
fc::Result validate_composition(CompositionSettings& settings) {
    ++settings.limit;
    return {};
}

// Builds base, named-section, range, and finite-map declarations through their distinct public overloads.
fc::SettingsSchema<CompositionSettings> composition_settings() {
    auto result = fc::settings<CompositionSettings>(
        fc::value("Enabled", &CompositionSettings::enabled, true)
            .description("Enables the contract behavior")
            .environment("FC_CONTRACT_ENABLED"),
        fc::value("Limit", &CompositionSettings::limit, std::int32_t{12})
            .range(std::int32_t{1}, std::int32_t{64})
            .description("Bounds the contract value")
            .environment("FC_CONTRACT_LIMIT"),
        fc::value("Ratio", &CompositionSettings::ratio, 1.5)
            .range(0.5, 2.0)
            .description("Scales the contract value")
            .environment("FC_CONTRACT_RATIO"),
        fc::value("Label", &CompositionSettings::label, std::string{"contract"})
            .max_length(32)
            .description("Names the contract value")
            .environment("FC_CONTRACT_LABEL"),
        fc::choice("Policy", &CompositionSettings::policy, ContractPolicy::Observe,
                   {{"Observe", ContractPolicy::Observe}, {"Enforce", ContractPolicy::Enforce}})
            .description("Selects the contract policy")
            .environment("FC_CONTRACT_POLICY"));

    result.sections.push_back(fc::section<CompositionSettings>(
        "Initializer",
        {fc::value("Text", &CompositionSettings::initializer_text, std::string{"initial"}).max_length(16)}));
    const std::array<fc::SettingEntry<CompositionSettings>, 1> range_entries{
        fc::value("Value", &CompositionSettings::range_value, std::uint32_t{7})
            .range(std::uint32_t{1}, std::uint32_t{9})};
    result.sections.push_back(fc::section<CompositionSettings>("Range", range_entries));
    result.sections.push_back(fc::section("Bindings", &CompositionSettings::bindings,
                                          {fc::StringSetting{.key = "Jump",
                                                             .default_value = "Space",
                                                             .description = "Primary jump binding",
                                                             .max_length = 16,
                                                             .environment = "FC_CONTRACT_JUMP"},
                                           fc::StringSetting{.key = "Roll", .default_value = "Alt"}}));
    constexpr std::array alias_declarations{
        fc::StringSetting{.key = "Primary", .default_value = "Jump", .max_length = 16}};
    result.sections.push_back(fc::section("Aliases", &CompositionSettings::aliases, alias_declarations));
    result.validate = &validate_composition;
    return result;
}

// Copies the completed object so the contract test can inspect typed reconstruction after Create returns.
class CompositionHandler final {
  public:
    using Settings = CompositionSettings;

    CompositionHandler(const fc::CreateContext&, const Settings& settings) {
        constructed_composition = settings;
    }
};

// Interface probes establish the positive and negative compile-time contract boundaries.
struct CounterV1 {
    static constexpr std::string_view id = "CounterV1";
    void* context{};
    void(FC_CALL* add)(void* context, std::uint32_t amount) noexcept = nullptr;
};

struct InvalidInterfaceId {
    static constexpr std::string_view id = "invalid-id";
    std::uint32_t value{};
};

struct RoutedV1 {
    static constexpr std::string_view id = "RoutedV1";
    fc::InterfaceFunction<std::int32_t(std::uint32_t) noexcept> read;
    fc::Observation<void(std::uint32_t) noexcept> changed;
};

static_assert(fc::InterfaceContract<CounterV1>);
static_assert(fc::InterfaceContract<RoutedV1>);
static_assert(!fc::InterfaceContract<InvalidInterfaceId>);
static_assert(sizeof(fc::InterfaceFunction<void() noexcept>) == sizeof(void*) * 2);
static_assert(sizeof(fc::Observation<void() noexcept>) == sizeof(void*) * 2);

// Native call aliases exercise compiler-derived and fully explicit physical layouts on each architecture.
using NativeFunction = int (*)(int) noexcept;

struct HiddenRecord {
    std::uint64_t first;
    std::uint64_t second;
};

#if defined(_M_IX86)
using ExplicitCall = fc::NativeCall<
    int(int), fc::abi::x86<fc::abi::args<fc::abi::stack<0>>, fc::abi::result<fc::abi::eax>, fc::abi::caller_cleanup>>;
using StdcallFunction = int(__stdcall*)(int);
using FastcallFunction = int(__fastcall*)(void*, int, float) noexcept;
using ThiscallFunction = int(__thiscall*)(void*, int) noexcept;
using HiddenCall =
    fc::NativeCall<HiddenRecord(int), fc::abi::x86<fc::abi::args<fc::abi::stack<4>>,
                                                   fc::abi::hidden_result<fc::abi::stack<0>>, fc::abi::caller_cleanup>>;
#else
using ExplicitCall = fc::NativeCall<int(int), fc::abi::x64<fc::abi::args<fc::abi::rcx>, fc::abi::result<fc::abi::rax>>>;
using HiddenCall =
    fc::NativeCall<HiddenRecord(int), fc::abi::x64<fc::abi::args<fc::abi::rdx>, fc::abi::hidden_result<fc::abi::rcx>>>;
#endif

// Redirect and observer fixtures provide concrete callable values for the authoring surface below.
int replacement(int value) noexcept {
    return value + 1;
}

struct ObservationState {
    std::uint64_t start;
};

static_assert(std::is_trivial_v<ObservationState>);

// One handler intentionally implements every optional lifecycle capability exposed by the public SDK.
class FullHandler {
  public:
    using Settings = PatchSettings;

    FullHandler(const fc::CreateContext&, const Settings&) {}
    ~FullHandler() noexcept = default;

    void plan(fc::Plan& plan) {
        // Named locations cover each semantic location class and both compiler-derived and explicit calls.
        const fc::DataLocation<std::uint16_t, 2> data{
            .rva = {0x1000}, .name = "contract data", .evidence = fc::expect(std::uint32_t{0})};
        const fc::FunctionLocation<NativeFunction> function{.rva = {0x2000}, .name = "contract function"};
        const fc::FunctionLocation<ExplicitCall> explicit_function{.rva = {0x2100}};
        const fc::FunctionLocation<HiddenCall> hidden_function{.rva = {0x2140}};
        const fc::CallLocation<NativeFunction> call{.rva = {0x2200}};
        const fc::CodeLocation code{.rva = {0x2300}};
        const fc::VtableLocation vtable{.rva = {0x2400}};

        // Requirements cover typed data, native calls, raw code, vtables, and compact RVA authoring.
        (void)plan.target();
        (void)plan.logger();
        (void)plan.require(data);
        (void)plan.require_mutable(data);
        (void)plan.require(function);
        (void)plan.require(explicit_function);
        (void)plan.require(hidden_function);
#if defined(_M_IX86)
        (void)plan.require(fc::FunctionLocation<StdcallFunction>{.rva = {0x2110}});
        (void)plan.require(fc::FunctionLocation<FastcallFunction>{.rva = {0x2120}});
        (void)plan.require(fc::FunctionLocation<ThiscallFunction>{.rva = {0x2130}});
#endif
        (void)plan.require(code, 4);
        (void)plan.require(vtable, sizeof(void*) * 2);
        (void)plan.require_at({0x2500}, 4, fc::exact_bytes({std::byte{0x90}}));

        // Mutations cover bytes, evidence, NOPs, and call/jump redirection targets.
        plan.write(fc::element(data, 0), std::uint16_t{128});
        const std::array replacement_bytes{std::byte{0x90}, std::byte{0x90}};
        plan.write_at({0x2600}, replacement_bytes, fc::masked_bytes({std::byte{0x00}}, {std::byte{0xff}}));
        plan.nop(code, 2);
        plan.nop_at({0x2700}, 2);
        (void)plan.redirect_call(call, &replacement);
        (void)plan.redirect_call_at<NativeFunction>({0x2800}, function);
        (void)plan.redirect_jump(code, &replacement);
        (void)plan.redirect_jump_at<NativeFunction>({0x2900}, function);

        // Symbolic allocations cover zero-initialized and author-initialized storage plus derived addresses.
        auto data_handle = plan.allocate_data<std::uint32_t>(4, "contract allocation");
        const std::array initial_values{std::uint32_t{1}, std::uint32_t{2}};
        auto initialized_handle =
            plan.allocate_data<std::uint32_t>(std::span{initial_values}, "initialized allocation");
        plan.write_at({0x2a00}, data_handle.base());
        plan.write_at({0x2b00}, fc::rel32(initialized_handle.element(1)));

        // Shared hook composition covers owners, individual observers, paired state, and instruction callbacks.
        (void)plan.hook(function, [](fc::Original<NativeFunction>, int value) noexcept {
            return value;
        });
        (void)plan.hook(explicit_function, [](fc::Original<ExplicitCall>, int value) noexcept {
            return value;
        });
        (void)plan.hook(hidden_function, [](fc::Original<HiddenCall>, int) noexcept {
            return HiddenRecord{};
        });
        plan.hook(code, [](fc::CpuContext&) noexcept {});
        plan.observe(function, fc::before([](int) noexcept {}));
        plan.observe(function, fc::after([](int, int) noexcept {}));
        plan.observe<ObservationState>(function, fc::before([](int, ObservationState& state) noexcept {
                                           state.start = 1;
                                       }),
                                       fc::after([](int, int, const ObservationState&) noexcept {}));
        plan.observe(code, fc::before([](const fc::CpuContext&) noexcept {}));
        plan.observe<ObservationState>(code, fc::before([](const fc::CpuContext&, ObservationState& state) noexcept {
                                           state.start = 1;
                                       }),
                                       fc::after([](const fc::CpuContext&, const ObservationState&) noexcept {}));
        plan.bind<CounterV1>("Provider", [](CounterV1) noexcept {});
    }

    [[nodiscard]] fc::Result prepare(fc::PrepareContext& context) {
        // Exercise immediate interfaces and trace preparation, retaining the channel as a real handler would.
        (void)context.logger();
        (void)context.find_interface<CounterV1>("Provider");
        auto trace = context.create_trace({.name = "contract", .capacity = 16, .max_record_size = 32});
        if (trace) {
            trace_ = std::move(*trace);
        }
        return {};
    }
    // No-op activation and update methods prove both optional callback signatures participate in adapter lowering.
    void activate(fc::ActivateContext&) noexcept {}
    void update(fc::UpdateContext&) noexcept {}
    void write_status(fc::StatusWriter& output) const noexcept {
        (void)output.add("Enabled", true);
        (void)output.add("Count", std::uint32_t{1});
        (void)output.add("Ratio", 1.0);
        (void)output.add("State", std::string_view{"ready"});
    }
    void query_interface(fc::InterfaceQuery& query) noexcept {
        query.provide(CounterV1{});
    }

  private:
    std::optional<fc::TraceChannel> trace_;
};

// Shared factories keep the lowering assertions focused on SDK composition rather than repeated fixture metadata.
fc::Support retail_support() {
    return fc::support({
        .layouts = {fc::TargetLayout::GameSpyRetail},
        .roles = fc::HostRole::Client,
        .image = fc::TargetImage::Game,
    });
}

fc::Plugin build_plugin() {
    return fc::plugin({
        .id = "ContractPlugin",
        .patches =
            {
                fc::patch<FullHandler>({
                    .id = "Full",
                    .name = "Full handler",
                    .settings = patch_settings(),
                    .supports = {retail_support()},
                }),
                fc::plan_patch(
                    {
                        .id = "Compact",
                        .name = "Compact handler",
                        .supports = {retail_support()},
                    },
                    [](fc::Plan& plan) {
                        plan.nop_at({0x2000}, 2);
                    }),
            },
    });
}

// This factory isolates completeness checks from the existing test of ordinary factory lowering.
fc::Plugin build_adapter_contract_plugin() {
    return fc::plugin({
        .id = "AdapterContractPlugin",
        .patches =
            {
                fc::patch<CompositionHandler>({.id = "Composition",
                                               .name = "Settings composition contract",
                                               .settings = composition_settings(),
                                               .supports = {retail_support()}}),
                fc::plan_patch(
                    {.id = "Allocation", .name = "Allocation preparation contract", .supports = {retail_support()}},
                    [](fc::Plan& plan) {
                        constexpr std::array initial_values{std::uint32_t{0x1122'3344}, std::uint32_t{0x5566'7788}};
                        (void)plan.allocate_data<std::uint32_t>(std::span{initial_values}, "Contract values");
                    }),
            },
    });
}

// The sink copies only the counts needed to prove the complete nested tree survived lowering.
struct Submission {
    std::uint32_t patch_count{};
    std::uint32_t support_count{};
    std::uint32_t setting_count{};
};

FC_SubmitResult FC_CALL submit(void* context, const FC_PluginDefinition* plugin) {
    auto& result = *static_cast<Submission*>(context);
    result.patch_count = plugin->patch_count;
    result.support_count = plugin->patches[0].support_count;
    result.setting_count = plugin->patches[0].setting_count;
    return FC_SUBMIT_ACCEPTED;
}

// Copies the two generated callback tables and settings metadata while their registration views are valid.
struct AdapterSubmission {
    std::array<FC_PatchCallbacks, 2> callbacks{};
    std::vector<FC_SettingDefinition> settings;
};

FC_SubmitResult FC_CALL submit_adapter_contract(void* context, const FC_PluginDefinition* plugin) {
    if (context == nullptr || plugin == nullptr || plugin->patch_count != 2 || plugin->patches == nullptr ||
        plugin->patches[0].support_count != 1 || plugin->patches[1].support_count != 1) {
        return FC_SUBMIT_REJECTED;
    }
    auto& result = *static_cast<AdapterSubmission*>(context);
    result.callbacks[0] = plugin->patches[0].supports[0].callbacks;
    result.callbacks[1] = plugin->patches[1].supports[0].callbacks;
    result.settings.assign(plugin->patches[0].settings, plugin->patches[0].settings + plugin->patches[0].setting_count);
    return FC_SUBMIT_ACCEPTED;
}

// The plan sink models framework-owned storage and copies initial bytes before the callback returns.
struct AllocationCapture {
    alignas(std::uint32_t) std::array<std::byte, sizeof(std::uint32_t) * 2> storage{};
    FC_DataHandle handle{7};
    std::uint32_t submissions{};
};

FC_SubmitResult FC_CALL capture_allocation(void* context, const FC_DataAllocationRequest* request,
                                           FC_DataHandle* output) {
    if (context == nullptr || request == nullptr || output == nullptr) {
        return FC_SUBMIT_REJECTED;
    }
    auto& capture = *static_cast<AllocationCapture*>(context);
    if (request->byte_size != capture.storage.size() || request->alignment != alignof(std::uint32_t) ||
        request->initial_bytes.size != capture.storage.size() || request->initial_bytes.data == nullptr) {
        return FC_SUBMIT_REJECTED;
    }
    std::memcpy(capture.storage.data(), request->initial_bytes.data, capture.storage.size());
    ++capture.submissions;
    *output = capture.handle;
    return FC_SUBMIT_ACCEPTED;
}

// The Prepare phase resolves only the handle admitted by the plan sink and returns its exact aligned extent.
FC_Bool FC_CALL resolve_allocation(void* context, FC_DataHandle handle, std::uintptr_t* address,
                                   std::uint64_t* byte_size) {
    if (context == nullptr || address == nullptr || byte_size == nullptr) {
        return FC_FALSE;
    }
    auto& capture = *static_cast<AllocationCapture*>(context);
    if (handle != capture.handle) {
        return FC_FALSE;
    }
    *address = reinterpret_cast<std::uintptr_t>(capture.storage.data());
    *byte_size = capture.storage.size();
    return FC_TRUE;
}

[[nodiscard]] std::string copy_text(FC_StringView value) {
    return {value.data, value.size};
}

// Releases the retained generated adapters even when a later contract assertion aborts the test body.
struct AdapterRegistrationRelease {
    ~AdapterRegistrationRelease() {
        fc::detail::release_registration<&build_adapter_contract_plugin>();
    }
};

} // namespace

TEST_CASE("the SDK factory lowers one complete plugin definition") {
    // Query and register through the generated native adapter rather than inspecting the C++ author model directly.
    const auto* api = fc::detail::query_plugin<&build_plugin>(FC_PLUGIN_ABI_GENERATION);
    REQUIRE(api != nullptr);

    Submission submission;
    const FC_HostApi host{.struct_size = sizeof(FC_HostApi)};
    const FC_RegistrySink registry{.struct_size = sizeof(FC_RegistrySink), .context = &submission, .submit = &submit};
    const FC_ErrorSink error{.struct_size = sizeof(FC_ErrorSink)};

    CHECK(api->register_plugin(&host, &registry, &error) == FC_CALL_OK);
    // Counts at three nesting levels prove the plugin, support, and typed setting trees all reached the C sink.
    CHECK(submission.patch_count == 2);
    CHECK(submission.support_count == 1);
    CHECK(submission.setting_count == 3);
}

TEST_CASE("settings composition and allocation preparation cross the generated SDK adapter") {
    constructed_composition.reset();
    AdapterSubmission submission;
    const FC_HostApi host{.struct_size = sizeof(FC_HostApi)};
    const FC_RegistrySink registry{
        .struct_size = sizeof(FC_RegistrySink), .context = &submission, .submit = &submit_adapter_contract};
    const auto* api = fc::detail::query_plugin<&build_adapter_contract_plugin>(FC_PLUGIN_ABI_GENERATION);
    REQUIRE(api != nullptr);
    REQUIRE(api->register_plugin(&host, &registry, nullptr) == FC_CALL_OK);
    const AdapterRegistrationRelease release;

    // The flattened order proves every helper and section overload reached the one native settings representation.
    REQUIRE(submission.settings.size() == 10);
    CHECK(copy_text(submission.settings[0].key) == "Enabled");
    CHECK(copy_text(submission.settings[0].environment) == "FC_CONTRACT_ENABLED");
    CHECK(submission.settings[1].has_range == FC_TRUE);
    CHECK(copy_text(submission.settings[2].environment) == "FC_CONTRACT_RATIO");
    CHECK(submission.settings[3].max_length == 32);
    CHECK(submission.settings[4].type == FC_SETTING_CHOICE);
    CHECK(submission.settings[4].choice_count == 2);
    CHECK(copy_text(submission.settings[5].section) == "Initializer");
    CHECK(copy_text(submission.settings[6].section) == "Range");
    CHECK(copy_text(submission.settings[7].section) == "Bindings");
    CHECK(copy_text(submission.settings[7].environment) == "FC_CONTRACT_JUMP");
    CHECK(copy_text(submission.settings[9].section) == "Aliases");

    // Create reconstructs the completed aggregate and finite maps through the same ordered values seen by production.
    std::vector<FC_SettingValue> values;
    values.reserve(submission.settings.size());
    for (const auto& setting : submission.settings) {
        values.push_back(setting.default_value);
    }
    const FC_TargetInfo target{
        .layout = FC_LAYOUT_GAMESPY_RETAIL,
        .role = FC_HOST_ROLE_CLIENT,
#if defined(_M_IX86)
        .architecture = FC_ARCH_X86,
#else
        .architecture = FC_ARCH_X64,
#endif
    };
    const FC_CreateContext create{.struct_size = sizeof(FC_CreateContext), .target = target};
    const FC_SettingsView settings{.struct_size = sizeof(FC_SettingsView),
                                   .values = values.data(),
                                   .count = static_cast<std::uint32_t>(values.size())};
    FC_PatchHandle composition{};
    REQUIRE(submission.callbacks[0].create(submission.callbacks[0].context, &create, &settings, nullptr,
                                           &composition) == FC_CALL_OK);
    REQUIRE(composition != nullptr);
    REQUIRE(constructed_composition.has_value());
    CHECK(constructed_composition->enabled);
    CHECK(constructed_composition->limit == 13);
    CHECK(constructed_composition->ratio == 1.5);
    CHECK(constructed_composition->label == "contract");
    CHECK(constructed_composition->policy == ContractPolicy::Observe);
    CHECK(constructed_composition->initializer_text == "initial");
    CHECK(constructed_composition->range_value == 7);
    REQUIRE(constructed_composition->bindings.entries().size() == 2);
    CHECK(constructed_composition->bindings.entries()[0].key == "Jump");
    const auto jump = constructed_composition->bindings.find("jump");
    const auto primary = constructed_composition->aliases.find("PRIMARY");
    REQUIRE(jump.has_value());
    REQUIRE(primary.has_value());
    CHECK(std::string{*jump} == "Space");
    CHECK(std::string{*primary} == "Jump");
    submission.callbacks[0].destroy(submission.callbacks[0].context, composition);

    // A settings-free compact patch still receives the valid empty Create view required by the native contract.
    const FC_SettingsView empty_settings{.struct_size = sizeof(FC_SettingsView)};
    FC_PatchHandle allocation{};
    REQUIRE(submission.callbacks[1].create(submission.callbacks[1].context, &create, &empty_settings, nullptr,
                                           &allocation) == FC_CALL_OK);
    REQUIRE(allocation != nullptr);
    AllocationCapture capture;
    const FC_PlanContext plan{.struct_size = sizeof(FC_PlanContext), .target = target};
    const FC_PlanSink sink{
        .struct_size = sizeof(FC_PlanSink), .context = &capture, .allocate_data = &capture_allocation};
    REQUIRE(submission.callbacks[1].plan(submission.callbacks[1].context, allocation, &plan, &sink, nullptr) ==
            FC_CALL_OK);
    REQUIRE(capture.submissions == 1);

    // The generated Prepare callback adapter begins storage lifetime even though the author supplied no such method.
    const FC_PrepareContext prepare{
        .struct_size = sizeof(FC_PrepareContext), .context = &capture, .resolve_data = &resolve_allocation};
    REQUIRE(submission.callbacks[1].prepare != nullptr);
    REQUIRE(submission.callbacks[1].prepare(submission.callbacks[1].context, allocation, &prepare, nullptr) ==
            FC_CALL_OK);
    const auto* prepared_values = reinterpret_cast<const std::uint32_t*>(capture.storage.data());
    CHECK(prepared_values[0] == 0x1122'3344);
    CHECK(prepared_values[1] == 0x5566'7788);
    submission.callbacks[1].destroy(submission.callbacks[1].context, allocation);
}

TEST_CASE("C++ support composition rejects duplicate layouts") {
    CHECK_THROWS_AS(fc::support({.layouts = {fc::TargetLayout::GameSpyRetail, fc::TargetLayout::GameSpyRetail},
                                 .roles = fc::HostRole::Client,
                                 .image = fc::TargetImage::Game}),
                    std::invalid_argument);
}

TEST_CASE("C++ composition rejects malformed required metadata") {
    CHECK_THROWS_AS(fc::support({.roles = fc::HostRole::Client, .image = fc::TargetImage::Game}),
                    std::invalid_argument);
    CHECK_THROWS_AS(fc::plugin({.id = "EmptyPlugin"}), std::invalid_argument);
    CHECK_THROWS_WITH(fc::plugin({.id = "fusioncutter"}), "FusionCutter is a reserved plugin ID");
}

TEST_CASE("calls using the compiler's native ABI lower to the selected architecture") {
#if defined(_M_IX86)
    // x86 rows distinguish stack cleanup and the register prefixes of stdcall, fastcall, and thiscall.
    const auto standard = fc::detail::native_call_storage<StdcallFunction>();
    REQUIRE(standard.arguments.size() == 1);
    CHECK(standard.call.cleanup == FC_STACK_CLEANUP_CALLEE);
    CHECK(standard.call.stack_size == 4);
    CHECK(standard.arguments[0].storage.kind == FC_NATIVE_STORAGE_STACK);

    const auto fast = fc::detail::native_call_storage<FastcallFunction>();
    REQUIRE(fast.arguments.size() == 3);
    CHECK(fast.arguments[0].storage.register_id == FC_REGISTER_ECX);
    CHECK(fast.arguments[1].storage.register_id == FC_REGISTER_EDX);
    CHECK(fast.arguments[2].storage.kind == FC_NATIVE_STORAGE_STACK);
    CHECK(fast.call.cleanup == FC_STACK_CLEANUP_CALLEE);

    const auto member = fc::detail::native_call_storage<ThiscallFunction>();
    REQUIRE(member.arguments.size() == 2);
    CHECK(member.arguments[0].storage.register_id == FC_REGISTER_ECX);
    CHECK(member.arguments[1].storage.kind == FC_NATIVE_STORAGE_STACK);
#else
    // x64 uses its unified register convention and never exposes caller/callee cleanup in normalized metadata.
    const auto native = fc::detail::native_call_storage<NativeFunction>();
    REQUIRE(native.arguments.size() == 1);
    CHECK(native.arguments[0].storage.register_id == FC_REGISTER_RCX);
    CHECK(native.call.cleanup == FC_STACK_CLEANUP_NONE);
#endif
}

TEST_CASE("default and moved-from trace channels remain harmless") {
    fc::TraceChannel source;
    fc::TraceChannel destination{std::move(source)};

    CHECK_FALSE(source.enabled());
    CHECK_FALSE(destination.enabled());
    CHECK_FALSE(source.try_write(std::span<const std::byte>{}));
    CHECK_FALSE(destination.try_write(std::span<const std::byte>{}));
}
