add_executable(rconserver_loader_tests
    loader_tests.cpp
)

target_include_directories(rconserver_loader_tests PRIVATE
    "${FC_SOURCE_ROOT}/include"
)

target_link_libraries(rconserver_loader_tests PRIVATE
    Catch2::Catch2
)

add_dependencies(rconserver_loader_tests
    fusioncutter_rconserver
    loader_core_stub
)

fc_configure_project_target(rconserver_loader_tests)
fc_set_artifact_directories(rconserver_loader_tests)

add_test(NAME loaders.rconserver
    COMMAND "${CMAKE_COMMAND}" -E env
        "FC_RCONSERVER_TEST_DLL=$<TARGET_FILE:fusioncutter_rconserver>"
        "FC_RCONSERVER_CORE_STUB=$<TARGET_FILE:loader_core_stub>"
        "$<TARGET_FILE:rconserver_loader_tests>"
)
