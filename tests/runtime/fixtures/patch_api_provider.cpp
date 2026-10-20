#include "patch_api_contract.hpp"
#include <fc/plugin.hpp>
#include <Windows.h>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>
#include <utility>

namespace {

// Captured settings let the parent check which INI and environment values reach late handler construction.
struct ProviderSettings {
    std::uint32_t from_ini = 20, from_environment = 30;
};

// Declares the two settings whose startup values must survive delayed handler construction.
auto provider_settings() {
    return fc::settings<ProviderSettings>(
        {fc::setting("from_ini", &ProviderSettings::from_ini, "Captured INI value."),
         fc::setting("from_environment", &ProviderSettings::from_environment, "Captured environment value.")
             .environment("FC_LATE_CAPTURE")});
}

// Publishes callable state from a separate DLL and retains consumer callbacks on its worker thread.
struct Provider {
    using Settings = ProviderSettings;

    // Reports captured settings when late construction is the selected child scenario.
    Provider(const fc::CreateContext &, const Settings &settings) {
        std::string mode;
        std::ifstream{"test.mode"} >> mode;
        if (mode == "late-settings") {
            std::ofstream{"factory.result", std::ios::app} << settings.from_ini << ' ' << settings.from_environment << '\n';
        }
    }

    std::jthread worker;

    // Empty custom status must not create a report entry for this provider.
    void write_status(fc::StatusContext &) const noexcept {}

    // Returns a recognizable result from a cross-DLL record argument.
    std::uint32_t count(fc::tests::api::Record record) noexcept {
        return record.tag + 7;
    }

    // Invokes the retained consumer callback inside a provider SEH handler to detect an escaped fault.
    static void enclosing_handler(const fc::ConsumerCallback<void() noexcept> &callback) noexcept {
        // Exit code 4 detects a fault escaping the runtime boundary into the provider's enclosing SEH handler.
        __try {
            static_cast<void>(callback.try_call());
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            ExitProcess(4);
        }
    }

    // Retains the binding on a worker; the installed consumer object must survive its eventual invocation.
    void dispatch(fc::ConsumerCallback<void() noexcept> callback) noexcept {
        // Wait for the child's request so the worker invokes the binding after startup.
        worker = std::jthread([callback](std::stop_token stop) {
            while (!stop.stop_requested() && !std::filesystem::exists("fault.request")) {
                Sleep(10);
            }
            if (!stop.stop_requested()) {
                enclosing_handler(callback);
            }
        });
    }

    // Publishes the bound table and records completion after a deliberate delay in the timing scenario.
    fc::Result<> prepare(fc::PrepareContext &context) {
        std::string mode;
        std::ifstream{"test.mode"} >> mode;
        if (mode == "timing-late") {
            // Record completion after slow preparation so observer updates can detect a stale clock sample.
            Sleep(120);
            std::ofstream{"preparation.finished"}
                << std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
        }

        // The runtime copies this local table; its bound provider object remains installed until exit.
        return context.publish(fc::tests::api::contract,
                               fc::tests::api::Table{fc::bind<&Provider::count>(*this), fc::bind<&Provider::dispatch>(*this)});
    }
};

// An independently installed handler measures the update sample after a late provider's preparation finishes.
struct TimingObserver {
    bool checked{};
    std::uint32_t updates{};

    // Reports update progress and checks the first time sample observed after provider preparation finishes.
    void update(const fc::UpdateContext &context) noexcept {
        std::ofstream{"progress.result"} << ++updates;
        if (checked) {
            return;
        }

        std::int64_t completed{};
        std::ifstream{"preparation.finished"} >> completed;
        if (!completed) {
            return;
        }

        // Compare the runtime's update sample with the provider's recorded preparation completion.
        checked = true;
        const auto now = std::chrono::duration_cast<std::chrono::nanoseconds>(context.now.time_since_epoch()).count();
        std::ofstream{"timing.result"} << (now >= completed ? "fresh" : "stale");
    }
};

// Declares immediate or image-dependent provider installation, with an independent observer for late scenarios.
fc::Plugin declaration() {
    std::string mode;
    std::ifstream{"test.mode"} >> mode;
    if (mode == "image-late" || mode == "timing-late" || mode == "late-settings" || mode == "image-incompatible") {
        // Extra image requirements postpone provider construction until the child loads the fixture modules.
        std::vector requirements{fc::ImageRequirement{"main", {"ApiFixture"}}, fc::ImageRequirement{"game", {"GameFixture"}}};
        auto variant =
            mode == "late-settings"
                ? fc::variant<Provider>({fc::ImageRequirement{"main", {"ApiFixture"}}, fc::ImageRequirement{"game", {"GameFixture"}},
                                         fc::ImageRequirement{"peer", {"PeerFixture"}}})
                : fc::variant<Provider>({requirements[0], requirements[1]});
        auto plugin = fc::Plugin{
            .id = "ApiProvider",
            .version = "1",
            .patches = {fc::Patch{.id = "Publish", .enabled = true, .settings = provider_settings(), .variants = {std::move(variant)}}}};
        // The observer installs before the waiting provider and measures updates after the later installation pass.
        plugin.patches.push_back(fc::Patch{.id = "Observe", .enabled = true, .variants = {fc::variant<TimingObserver>({"ApiFixture"})}});
        return plugin;
    }

    return {.id = "ApiProvider",
            .version = "1",
            .patches = {fc::Patch{
                .id = "Publish", .enabled = true, .settings = provider_settings(), .variants = {fc::variant<Provider>({"ApiFixture"})}}}};
}
} // namespace
FC_PLUGIN(declaration)
