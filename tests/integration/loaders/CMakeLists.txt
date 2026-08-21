add_executable(fc_loader_child
    loader_child.cpp
)
fc_configure_project_target(fc_loader_child)
fc_set_artifact_directories(fc_loader_child)
target_link_libraries(fc_loader_child PRIVATE FusionCutter::SDK)

if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    target_compile_definitions(fc_loader_child PRIVATE
        FC_CLIENT_LOADER_PATH=L"$<TARGET_FILE:FusionCutterBattlefront2>"
        FC_CLIENT_LOADER_NAME=L"Battlefront2.dll"
        FC_CLIENT_AUXILIARY_PATH=L"$<TARGET_FILE:fc_loader_original_stub>"
        FC_CLIENT_LOADER_KIND=FC_LOADER_KIND_BATTLEFRONT2
        FC_CLIENT_CHAIN_RESULT=FC_DIRECT_INPUT_CHAIN_NOT_APPLICABLE
    )
    add_dependencies(fc_loader_child FusionCutterBattlefront2 fc_loader_original_stub)
else()
    target_compile_definitions(fc_loader_child PRIVATE
        FC_CLIENT_LOADER_PATH=L"$<TARGET_FILE:FusionCutterDInput8>"
        FC_CLIENT_LOADER_NAME=L"dinput8.dll"
        FC_CLIENT_AUXILIARY_PATH=L"$<TARGET_FILE:fc_loader_chain_stub>"
        FC_CLIENT_LOADER_KIND=FC_LOADER_KIND_DINPUT8
        FC_CLIENT_CHAIN_RESULT=FC_DIRECT_INPUT_CHAIN_LOADED
    )
    add_dependencies(fc_loader_child FusionCutterDInput8 fc_loader_chain_stub)
endif()

target_compile_definitions(fc_loader_child PRIVATE
    FC_CORE_STUB_PATH=L"$<TARGET_FILE:fc_loader_core_stub>"
    FC_SERVER_LOADER_PATH=L"$<TARGET_FILE:FusionCutterRconServer>"
    FC_SERVER_LOADER_NAME=L"$<TARGET_FILE_NAME:FusionCutterRconServer>"
)
add_dependencies(fc_loader_child fc_loader_core_stub FusionCutterRconServer)

fc_add_catch_test(fc_loader_tests
    NAME fc_loaders
    LABEL integration
    SOURCES loader_tests.cpp
    LIBRARIES FusionCutter::SDK
)
target_compile_definitions(fc_loader_tests PRIVATE
    FC_LOADER_CHILD_PATH="$<TARGET_FILE:fc_loader_child>"
    FC_PRODUCTION_CORE_PATH=L"$<TARGET_FILE:FusionCutter>"
)
add_dependencies(fc_loader_tests fc_loader_child FusionCutter)
