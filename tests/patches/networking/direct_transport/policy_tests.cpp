#include "server/policy.hpp"

#include <Windows.h>

#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <string>
#include <utility>

namespace direct_transport = fusioncutter::patches::direct_transport;

namespace {

class EnvironmentRestore {
  public:
    explicit EnvironmentRestore(std::optional<std::string> value) : value_(std::move(value)) {}

    ~EnvironmentRestore() {
        SetEnvironmentVariableA("BF2_DIRECT_POLICY", value_.has_value() ? value_->c_str() : nullptr);
    }

  private:
    std::optional<std::string> value_;
};

} // namespace

TEST_CASE("Direct Transport environment policy overrides only its configured server policy",
          "[patches][direct_transport]") {
    constexpr auto variable = "BF2_DIRECT_POLICY";
    auto original = fusioncutter::read_environment_value<std::string>(variable);
    REQUIRE(original.has_value());
    const EnvironmentRestore restore(std::move(*original));

    REQUIRE(SetEnvironmentVariableA(variable, nullptr));
    auto policy = direct_transport::server::resolve_policy(direct_transport::Policy::RequireDirectAll);
    REQUIRE(policy.has_value());
    CHECK(*policy == direct_transport::Policy::RequireDirectAll);

    REQUIRE(SetEnvironmentVariableA(variable, "Disabled"));
    policy = direct_transport::server::resolve_policy(direct_transport::Policy::PreferDirect);
    REQUIRE(policy.has_value());
    CHECK(*policy == direct_transport::Policy::Disabled);

    REQUIRE(SetEnvironmentVariableA(variable, "not-a-policy"));
    CHECK_FALSE(direct_transport::server::resolve_policy(direct_transport::Policy::PreferDirect).has_value());
}
