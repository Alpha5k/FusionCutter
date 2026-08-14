if(NOT FC_ARCHITECTURE STREQUAL "x86")
    return()
endif()

add_executable(network_pipeline_abi_tests
    abi_tests.cpp
    "${FC_SOURCE_ROOT}/src/patches/pipelines/network/pipeline.cpp"
)

target_include_directories(network_pipeline_abi_tests PRIVATE
    "${FC_SOURCE_ROOT}/include"
)

target_compile_definitions(network_pipeline_abi_tests PRIVATE
    FC_NETWORK_PIPELINE_ABI_TEST
)

target_link_libraries(network_pipeline_abi_tests PRIVATE
    Catch2::Catch2WithMain
    fusioncutter_patching
    Ws2_32
)

fc_configure_project_target(network_pipeline_abi_tests)
fc_set_artifact_directories(network_pipeline_abi_tests)

add_test(NAME patches.network_pipeline_abi COMMAND network_pipeline_abi_tests)
