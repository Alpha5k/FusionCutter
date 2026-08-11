add_library(battlefront2_original_stub SHARED
    original_stub.cpp
    original_stub.def
    original_stub.hpp
)

fc_configure_project_target(battlefront2_original_stub)
fc_set_artifact_directories(battlefront2_original_stub)

add_executable(battlefront2_loader_tests
    loader_tests.cpp
    original_stub.hpp
)

target_include_directories(battlefront2_loader_tests PRIVATE
    "${FC_SOURCE_ROOT}/include"
)

target_link_libraries(battlefront2_loader_tests PRIVATE
    Catch2::Catch2
)

add_dependencies(battlefront2_loader_tests
    battlefront2_original_stub
    fusioncutter_battlefront2
    loader_core_stub
)

fc_configure_project_target(battlefront2_loader_tests)
fc_set_artifact_directories(battlefront2_loader_tests)

add_test(NAME loaders.battlefront2
    COMMAND "${CMAKE_COMMAND}" -E env
        "FC_BATTLEFRONT2_TEST_DLL=$<TARGET_FILE:fusioncutter_battlefront2>"
        "FC_BATTLEFRONT2_ORIGINAL_STUB=$<TARGET_FILE:battlefront2_original_stub>"
        "FC_BATTLEFRONT2_CORE_STUB=$<TARGET_FILE:loader_core_stub>"
        "$<TARGET_FILE:battlefront2_loader_tests>"
)
