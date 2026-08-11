add_library(dinput8_chain_stub SHARED
    chain_stub.cpp
    chain_stub.def
)

fc_configure_project_target(dinput8_chain_stub)
fc_set_artifact_directories(dinput8_chain_stub)

add_executable(dinput8_loader_tests
    loader_tests.cpp
)

target_include_directories(dinput8_loader_tests PRIVATE
    "${FC_SOURCE_ROOT}/include"
)

target_link_libraries(dinput8_loader_tests PRIVATE
    Catch2::Catch2
)

add_dependencies(dinput8_loader_tests
    dinput8_chain_stub
    fusioncutter_dinput8
    loader_core_stub
)

fc_configure_project_target(dinput8_loader_tests)
fc_set_artifact_directories(dinput8_loader_tests)

add_test(NAME loaders.dinput8
    COMMAND "${CMAKE_COMMAND}" -E env
        "FC_DINPUT8_TEST_DLL=$<TARGET_FILE:fusioncutter_dinput8>"
        "FC_DINPUT8_CORE_STUB=$<TARGET_FILE:loader_core_stub>"
        "FC_DINPUT8_CHAIN_STUB=$<TARGET_FILE:dinput8_chain_stub>"
        "$<TARGET_FILE:dinput8_loader_tests>"
)
