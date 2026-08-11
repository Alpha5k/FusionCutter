add_library(quill_module SHARED
    module.cpp
    module_api.hpp
)

target_link_libraries(quill_module PRIVATE
    quill::quill
)

fc_configure_project_target(quill_module)
fc_set_artifact_directories(quill_module)

add_executable(quill_probe
    module_api.hpp
    probe.cpp
)

fc_configure_project_target(quill_probe)
fc_set_artifact_directories(quill_probe)

add_executable(quill_tests
    quill_tests.cpp
)

target_link_libraries(quill_tests PRIVATE
    Catch2::Catch2WithMain
)

fc_configure_project_target(quill_tests)
fc_set_artifact_directories(quill_tests)

add_dependencies(quill_tests quill_module quill_probe)

add_test(NAME core.quill
    COMMAND "${CMAKE_COMMAND}" -E env
        "FC_QUILL_MODULE=$<TARGET_FILE:quill_module>"
        "FC_QUILL_PROBE=$<TARGET_FILE:quill_probe>"
        "$<TARGET_FILE:quill_tests>"
)

set_tests_properties(core.quill PROPERTIES TIMEOUT 30)
