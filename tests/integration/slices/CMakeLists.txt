fc_add_plugin_tests(fc_simple_write_slice_author_tests
    PLUGINS
        fc_simple_write_slice
    SOURCES
        simple_write_author_tests.cpp
)
target_include_directories(fc_simple_write_slice_author_tests PRIVATE
    "${FC_SOURCE_ROOT}/tests/fixtures/slices"
    "${FC_SOURCE_ROOT}/tests/fixtures/slices/simple_write"
)

fc_add_catch_test(fc_simple_write_slice_integration_tests
    NAME fc_simple_write_slice_integration
    LABEL integration
    SOURCES simple_write_integration_tests.cpp
    LIBRARIES FusionCutter::Testing
)
target_include_directories(fc_simple_write_slice_integration_tests PRIVATE
    "${FC_SOURCE_ROOT}/src/core"
    "${FC_SOURCE_ROOT}/tests/fixtures/native"
    "${FC_SOURCE_ROOT}/tests/fixtures/slices/simple_write"
)
target_compile_definitions(fc_simple_write_slice_integration_tests PRIVATE
    FC_SIMPLE_WRITE_PLUGIN_PATH=L"$<TARGET_FILE:fc_simple_write_slice>"
    FC_SLICE_BUNDLED_CORE_PATH=L"$<TARGET_FILE:fc_slice_bundled_core>"
    FC_SLICE_EXTERNAL_CORE_PATH=L"$<TARGET_FILE:fc_slice_external_core>"
    FC_SLICE_HOST_PATH=L"$<TARGET_FILE:fc_slice_host>"
    FC_VERIFIER_PATH=L"$<TARGET_FILE:fusioncutter_verify>"
)
add_dependencies(fc_simple_write_slice_integration_tests
    fc_simple_write_slice
    fc_slice_bundled_core
    fc_slice_external_core
    fc_slice_host
    fusioncutter_verify
)

fc_add_catch_test(fc_simple_write_slice_bundled_tests
    NAME fc_simple_write_slice_bundled
    LABEL integration
    SOURCES simple_write_bundled_tests.cpp
    LIBRARIES fc_simple_write_bundle_testing
)
target_include_directories(fc_simple_write_slice_bundled_tests PRIVATE
    "${FC_SOURCE_ROOT}/tests/fixtures/slices"
    "${FC_SOURCE_ROOT}/tests/fixtures/slices/simple_write"
)

fc_add_plugin_tests(fc_relocated_plan_slice_author_tests
    PLUGINS
        fc_relocated_plan_slice
    SOURCES
        relocated_plan_author_tests.cpp
)
target_include_directories(fc_relocated_plan_slice_author_tests PRIVATE
    "${FC_SOURCE_ROOT}/tests/fixtures/slices"
    "${FC_SOURCE_ROOT}/tests/fixtures/slices/relocated_plan"
)

fc_add_catch_test(fc_relocated_plan_slice_integration_tests
    NAME fc_relocated_plan_slice_integration
    LABEL integration
    SOURCES relocated_plan_integration_tests.cpp
    LIBRARIES FusionCutter::Testing
)
target_include_directories(fc_relocated_plan_slice_integration_tests PRIVATE
    "${FC_SOURCE_ROOT}/src/core"
    "${FC_SOURCE_ROOT}/tests/fixtures/native"
    "${FC_SOURCE_ROOT}/tests/fixtures/slices/relocated_plan"
)
target_compile_definitions(fc_relocated_plan_slice_integration_tests PRIVATE
    FC_RELOCATED_PLUGIN_PATH=L"$<TARGET_FILE:fc_relocated_plan_slice>"
    FC_VERIFIER_PATH=L"$<TARGET_FILE:fusioncutter_verify>"
)
add_dependencies(fc_relocated_plan_slice_integration_tests
    fc_relocated_plan_slice
    fusioncutter_verify
)

fc_add_catch_test(fc_runtime_slice_integration_tests
    NAME fc_runtime_slice_integration
    LABEL integration
    SOURCES runtime_slice_tests.cpp
)
target_include_directories(fc_runtime_slice_integration_tests PRIVATE
    "${FC_SOURCE_ROOT}/tests/fixtures/slices"
)
target_compile_definitions(fc_runtime_slice_integration_tests PRIVATE
    FC_RUNTIME_LATE_PARTICIPANT_PATH=L"$<TARGET_FILE:fc_runtime_late_participant_slice>"
    FC_RUNTIME_OBSERVER_PATH=L"$<TARGET_FILE:fc_runtime_observer_slice>"
    FC_RUNTIME_PROVIDER_PATH=L"$<TARGET_FILE:fc_runtime_provider_slice>"
    FC_SLICE_CORE_PATH=L"$<TARGET_FILE:fc_slice_external_core>"
    FC_SLICE_HOST_PATH=L"$<TARGET_FILE:fc_slice_host>"
    FC_SLICE_LATE_IMAGE_PATH=L"$<TARGET_FILE:fc_slice_late_image>"
)
add_dependencies(fc_runtime_slice_integration_tests
    fc_runtime_late_participant_slice
    fc_runtime_observer_slice
    fc_runtime_provider_slice
    fc_slice_external_core
    fc_slice_host
    fc_slice_late_image
)
