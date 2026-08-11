add_executable(core_api_tests
    core_api_tests.cpp
)

target_include_directories(core_api_tests PRIVATE
    "${FC_SOURCE_ROOT}/include"
)

target_compile_definitions(core_api_tests PRIVATE
    FC_EXPECTED_ROLE_MASK=${FC_CORE_ROLE_MASK}
)

target_link_libraries(core_api_tests PRIVATE
    Catch2::Catch2WithMain
)

add_dependencies(core_api_tests fusioncutter_core)

fc_configure_project_target(core_api_tests)
fc_set_artifact_directories(core_api_tests)

add_test(NAME loaders.core_api
    COMMAND "${CMAKE_COMMAND}" -E env
        "FC_CORE_TEST_DLL=$<TARGET_FILE:fusioncutter_core>"
        "$<TARGET_FILE:core_api_tests>"
)

add_library(loader_core_stub SHARED
    core_stub.cpp
    core_stub.def
)

target_include_directories(loader_core_stub PRIVATE
    "${FC_SOURCE_ROOT}/include"
)

fc_configure_project_target(loader_core_stub)
fc_set_artifact_directories(loader_core_stub)

if(TARGET fusioncutter_dinput8)
    add_subdirectory(dinput8)
endif()

if(TARGET fusioncutter_battlefront2)
    add_subdirectory(battlefront2)
endif()

if(TARGET fusioncutter_rconserver)
    add_subdirectory(rconserver)
endif()
