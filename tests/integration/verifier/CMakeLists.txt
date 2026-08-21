fc_add_catch_test(fc_verifier_tests
    NAME fc_verifier
    LABEL integration
    SOURCES verifier_tests.cpp
    LIBRARIES FusionCutter::Testing
)

target_include_directories(fc_verifier_tests PRIVATE
    "${FC_SOURCE_ROOT}/src/core"
)

target_compile_definitions(fc_verifier_tests PRIVATE
    FC_VERIFIER_PATH=L"$<TARGET_FILE:fusioncutter_verify>"
)

add_dependencies(fc_verifier_tests fusioncutter_verify)
