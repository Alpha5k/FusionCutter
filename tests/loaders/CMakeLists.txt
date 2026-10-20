# Replace the runtime with a DLL that records startup arguments and selects success or failure.
add_library(fc_loader_core_stub SHARED
    fixtures/core_stub.cpp
    fixtures/core_stub.def
)
fc_configure_project_target(fc_loader_core_stub)
target_link_libraries(fc_loader_core_stub PRIVATE FusionCutter::SDK)

# Record the plugin discovery thread when the dedicated loader starts its embedded runtime.
if(TARGET FusionCutterRconServer)
    add_library(fc_startup_probe SHARED fixtures/startup_probe.cpp)
    fc_configure_project_target(fc_startup_probe)
    target_link_libraries(fc_startup_probe PRIVATE FusionCutter::SDK)
endif()

if(TARGET FusionCutterBattlefront2)
    # The Classic proxy forwards its game entry point to this original-module stand-in.
    add_library(fc_loader_original_stub SHARED
        fixtures/original_stub.cpp
        fixtures/original_stub.def
    )
    fc_configure_project_target(fc_loader_original_stub)
else()
    # Partial and full export tables exercise DirectInput's optional exports and forwarding fallbacks.
    add_library(fc_loader_chain_stub SHARED
        fixtures/chain_stub.cpp
        fixtures/chain_stub.def
    )
    fc_configure_project_target(fc_loader_chain_stub)

    add_library(fc_loader_full_chain_stub SHARED fixtures/chain_stub.cpp fixtures/full_chain_stub.def)
    fc_configure_project_target(fc_loader_full_chain_stub)
endif()

# Loader cases run in separate processes because loading a runtime changes process-wide state.
add_executable(fc_loader_child
    loader_child.cpp
    runtime_boundary.cpp
)
fc_configure_project_target(fc_loader_child)
target_link_libraries(fc_loader_child PRIVATE FusionCutter::SDK)

# Match the client test driver and auxiliary DLLs to the production loader for this architecture.
if(TARGET FusionCutterBattlefront2)
    target_sources(fc_loader_child PRIVATE client_classic.cpp)
    target_compile_definitions(fc_loader_child PRIVATE
        FC_CLIENT_LOADER_PATH=L"$<TARGET_FILE:FusionCutterBattlefront2>"
        FC_CLIENT_LOADER_NAME=L"Battlefront2.dll"
        FC_BF1_LOADER_PATH=L"$<TARGET_FILE:FusionCutterBattlefront1>"
        FC_CLIENT_AUXILIARY_PATH=L"$<TARGET_FILE:fc_loader_original_stub>"
    )
    add_dependencies(fc_loader_child FusionCutterBattlefront1 FusionCutterBattlefront2 fc_loader_original_stub)
else()
    target_sources(fc_loader_child PRIVATE client_dinput8.cpp)
    target_compile_definitions(fc_loader_child PRIVATE
        FC_CLIENT_LOADER_PATH=L"$<TARGET_FILE:FusionCutterDInput8>"
        FC_CLIENT_LOADER_NAME=L"dinput8.dll"
        FC_CLIENT_AUXILIARY_PATH=L"$<TARGET_FILE:fc_loader_chain_stub>"
        FC_FULL_CHAIN_PATH=L"$<TARGET_FILE:fc_loader_full_chain_stub>"
    )
    add_dependencies(fc_loader_child FusionCutterDInput8 fc_loader_chain_stub fc_loader_full_chain_stub)
endif()

# The child stages these DLLs by path; explicit dependencies make them available when building fc_tests.
target_compile_definitions(fc_loader_child PRIVATE
    FC_CORE_STUB_PATH=L"$<TARGET_FILE:fc_loader_core_stub>"
    FC_PRODUCTION_CORE_PATH=L"$<TARGET_FILE:FusionCutter>"
    FC_NO_EXPORT_PATH=L"$<TARGET_FILE:fc_fixture_no_export>"
)
add_dependencies(fc_loader_child fc_loader_core_stub FusionCutter fc_fixture_no_export)

if(TARGET FusionCutterRconServer)
    target_sources(fc_loader_child PRIVATE combined_startup.cpp)
    target_compile_definitions(fc_loader_child PRIVATE
        FC_TEST_DEDICATED
        FC_STARTUP_PROBE_PATH=L"$<TARGET_FILE:fc_startup_probe>"
        FC_SERVER_LOADER_PATH=L"$<TARGET_FILE:FusionCutterRconServer>"
        FC_SERVER_LOADER_NAME=L"$<TARGET_FILE_NAME:FusionCutterRconServer>"
    )
    target_compile_definitions(fc_tests PRIVATE FC_TEST_DEDICATED)
    add_dependencies(fc_loader_child FusionCutterRconServer fc_startup_probe)
endif()

target_compile_definitions(fc_tests PRIVATE FC_LOADER_CHILD_PATH="$<TARGET_FILE:fc_loader_child>")
add_dependencies(fc_tests fc_loader_child)
