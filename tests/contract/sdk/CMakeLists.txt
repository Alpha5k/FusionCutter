fc_add_catch_test(fc_sdk_contract_tests
    NAME fc_sdk_contract
    LABEL fast
    SOURCES sdk_contract.cpp
    LIBRARIES FusionCutter::SDK
)

# Build one factory through external and bundled plugin bridges to compile both registration forms.
fc_add_plugin(fc_sdk_external_contract
    FACTORY sdk_fixture::build_plugin
    SOURCES plugin_factory.cpp
)

include(fc_bundles)
fc_bundle_plugin(fc_sdk_bundle_contract
    FACTORY sdk_fixture::build_plugin
    SOURCES plugin_factory.cpp
)

add_dependencies(fc_sdk_contract_tests
    fc_sdk_external_contract
    fc_sdk_bundle_contract
)

fc_add_catch_test(fc_testing_contract_tests
    NAME fc_testing_contract
    LABEL fast
    SOURCES testing_contract.cpp
    LIBRARIES FusionCutter::Testing
)

target_include_directories(fc_testing_contract_tests PRIVATE
    "${FC_SOURCE_ROOT}/src/core"
)
