#include "patch_api_contract.hpp"
#include <fc/plugin.hpp>
#include <fc/reporting.hpp>
#include <Windows.h>
#include <cstdint>
#include <memory>
#include <string>
#include <filesystem>
#include <fstream>

namespace {

// Acquires the provider table and reports lifecycle stages or triggers the child process's selected fault.
struct Consumer {
    std::uint32_t value{}, available{};
    std::string mode;
    fc::ConsumerCallback<void() noexcept> callback;
    fc::tests::api::Table provider;
    // Stage bits and thread identity let the parent verify serialized control callbacks through status output.
    DWORD control_thread = GetCurrentThreadId();
    mutable unsigned stages = 1;
    mutable bool same_thread = true;

    // Adds a lifecycle stage and checks that it ran on the handler's construction thread.
    void record(unsigned stage) const noexcept {
        stages |= stage;
        same_thread &= GetCurrentThreadId() == control_thread;
    }

    // Reads the child process's scenario before any lifecycle callback runs.
    Consumer() {
        std::ifstream input{"test.mode"};
        input >> mode;
    }

    // Triggers a native fault inside author code for runtime containment and attribution tests.
    void fault() noexcept {
        *static_cast<volatile unsigned *>(nullptr) = 1;
    }

    // Records entry into planning for the lifecycle thread check.
    void plan(fc::Plan &) {
        record(2);
    }

    // Connects the selected API contract and retains a notification, or triggers the selected preparation fault.
    fc::Result<> prepare(fc::PrepareContext &context) {
        record(4);
        if (mode == "explicit-fault") {
            fc::fatal(fc::Error{"Deliberate consumer fatal", 123});
        }

        callback = context.callback<&Consumer::fault>(*this);
        // Request incompatible contracts with the same API ID to test runtime signature and layout checks.
        if (mode == "signature") {
            auto result = context.get("ApiProvider.Publish", fc::tests::api::changed_signature);
            return result ? fc::Result<>{} : std::unexpected(result.error());
        }
        if (mode == "layout") {
            auto result = context.get("ApiProvider.Publish", fc::tests::api::changed_layout);
            return result ? fc::Result<>{} : std::unexpected(result.error());
        }

        auto result = context.get("ApiProvider.Publish", fc::tests::api::contract);
        if (!result) {
            return std::unexpected(result.error());
        }

        // Retain the copied function table for activation and verify an ordinary cross-DLL call during Prepare.
        provider = *result;
        value = result->count({{}, 35});

        // The notification records its lifecycle stage and calls the provider using the retained text length.
        // This DLL's handler capture storage owns and destroys the nontrivial capture.
        return context.on_available("ApiProvider.Publish", fc::tests::api::contract,
                                    [this, text = std::make_shared<std::string>("retained")](const fc::tests::api::Table &table) noexcept {
                                        record(16);
                                        available += table.count({{}, static_cast<std::uint32_t>(text->size())});
                                    });
    }

    // Records activation and gives fault scenarios a retained callback for the provider worker.
    void activate(fc::ActivateContext &) noexcept {
        record(8);
        if (mode == "consumer-fault") {
            // Dispatch retains this binding until the child signals the provider worker after startup.
            provider.dispatch(callback);
        }
    }

    // Records installed updates for the lifecycle thread check.
    void update(const fc::UpdateContext &) noexcept {
        record(32);
    }

    // Exposes API results and lifecycle thread observations to the parent process.
    void write_status(fc::StatusContext &status) const noexcept {
        record(64);
        status.line("DLL API value={} available={}", value, available);
        status.line("Control callbacks: same_thread={} stages={}", same_thread, stages);
    }
};

// Declares the consumer with a required provider dependency for API retrieval during preparation.
fc::Plugin declaration() {
    return {
        .id = "ApiConsumer",
        .version = "1",
        .patches = {fc::Patch{
            .id = "Read", .enabled = true, .depends_on = {"ApiProvider.Publish"}, .variants = {fc::variant<Consumer>({"ApiFixture"})}}}};
}
} // namespace
FC_PLUGIN(declaration)
