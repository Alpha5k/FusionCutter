include("${CMAKE_CURRENT_LIST_DIR}/../cmake/project.cmake")
fc_initialize_build()

include("${CMAKE_CURRENT_LIST_DIR}/../cmake/dependencies.cmake")
fc_add_test_dependencies()

# Catch2's compiled string-view formatter must use the same language feature as its callers.
target_compile_features(Catch2 PRIVATE cxx_std_17)

# Lua 5.0.2 tests use the game's float number type; production calls the Lua implementation in the executable.
# Source: lua.org/ftp/lua-5.0.2.tar.gz, SHA-256 a6c85d85f912e1c321723084389d63dee7660b81b8292452b190ea7190dd73bc.
set(fc_lua_source "${FC_SOURCE_ROOT}/vendor/Lua")
add_library(fc_test_lua STATIC
    "${fc_lua_source}/src/lapi.c"
    "${fc_lua_source}/src/lcode.c"
    "${fc_lua_source}/src/ldebug.c"
    "${fc_lua_source}/src/ldo.c"
    "${fc_lua_source}/src/ldump.c"
    "${fc_lua_source}/src/lfunc.c"
    "${fc_lua_source}/src/lgc.c"
    "${fc_lua_source}/src/llex.c"
    "${fc_lua_source}/src/lmem.c"
    "${fc_lua_source}/src/lobject.c"
    "${fc_lua_source}/src/lopcodes.c"
    "${fc_lua_source}/src/lparser.c"
    "${fc_lua_source}/src/lstate.c"
    "${fc_lua_source}/src/lstring.c"
    "${fc_lua_source}/src/ltable.c"
    "${fc_lua_source}/src/ltm.c"
    "${fc_lua_source}/src/lundump.c"
    "${fc_lua_source}/src/lvm.c"
    "${fc_lua_source}/src/lzio.c"
    "${fc_lua_source}/src/lib/lauxlib.c"
    "${fc_lua_source}/src/lib/lbaselib.c"
    "${fc_lua_source}/src/lib/lmathlib.c"
    "${fc_lua_source}/src/lib/lstrlib.c"
    "${fc_lua_source}/src/lib/ltablib.c"
)
target_include_directories(fc_test_lua PUBLIC "${fc_lua_source}/include" PRIVATE "${fc_lua_source}/src")
target_compile_definitions(fc_test_lua PUBLIC LUA_NUMBER=float "LUA_NUMBER_SCAN=\"%f\"")

add_library(fc_fixture_plugin SHARED runtime/fixtures/declaration_fixture.cpp)
fc_configure_project_target(fc_fixture_plugin)
target_link_libraries(fc_fixture_plugin PRIVATE FusionCutter::SDK)

# This DLL tests a missing FC_GetPlugin export and compiles the ABI header as C11.
add_library(fc_fixture_no_export SHARED runtime/fixtures/no_export.c)
fc_configure_project_target(fc_fixture_no_export)
target_link_libraries(fc_fixture_no_export PRIVATE FusionCutter::SDK)
set_target_properties(fc_fixture_no_export PROPERTIES C_EXTENSIONS OFF C_STANDARD 11 C_STANDARD_REQUIRED ON)

add_library(fc_bootstrap_probe SHARED loaders/fixtures/bootstrap_probe.cpp)
fc_configure_project_target(fc_bootstrap_probe)
target_link_libraries(fc_bootstrap_probe PRIVATE FusionCutter::SDK)

add_executable(fc_tests
    runtime/plugin_tests.cpp
    runtime/image_tests.cpp
    runtime/configuration_tests.cpp
    runtime/selection_tests.cpp
    runtime/dependency_tests.cpp
    runtime/patch_api_tests.cpp
    runtime/lifecycle_tests.cpp
    runtime/integration_tests.cpp

    patches/lua_bindings/interpreter.cpp
    patches/lua_bindings/fixture.cpp
    patches/lua_bindings/frame_tests.cpp
    patches/lua_bindings/binding_tests.cpp
    patches/lua_multiplayer/fixture.cpp
    patches/lua_multiplayer/protocol_tests.cpp
    patches/lua_multiplayer/service_tests.cpp
    patches/rcon_server/protocol_tests.cpp

    game/game_tests.cpp
    multiplayer/protocol_tests.cpp
    multiplayer/service_tests.cpp
    lobby/service_tests.cpp

    native/bridge_tests.cpp
    native/plan_tests.cpp
    native/instruction_tests.cpp
    native/observer_tests.cpp
    native/hook_tests.cpp

    reporting/logging_tests.cpp
    reporting/status_tests.cpp
    reporting/tracing_tests.cpp
    reporting/crash_tests.cpp

    verifier/cli_tests.cpp
    verifier/offline_validation_tests.cpp
)

target_include_directories(fc_tests PRIVATE "${FC_SOURCE_ROOT}/patches/rcon_server")
target_include_directories(fc_tests PRIVATE "${fc_lua_source}/src")

# Exercise reporting calls across the DLL boundary with a separately compiled SDK handler.
add_library(fc_reporting_fixture SHARED reporting/sdk_fixture.cpp)
fc_configure_project_target(fc_reporting_fixture)
target_link_libraries(fc_reporting_fixture PRIVATE FusionCutter::SDK ws2_32)
target_compile_definitions(fc_tests PRIVATE FC_REPORTING_FIXTURE="$<TARGET_FILE:fc_reporting_fixture>")
add_dependencies(fc_tests fc_reporting_fixture)

# Run runtime startup in a child process so each case gets its own process-wide state.
add_executable(fc_control_child runtime/fixtures/control_child.cpp)
fc_configure_project_target(fc_control_child)
target_link_libraries(fc_control_child PRIVATE fusioncutter_runtime)
target_compile_definitions(fc_control_child PRIVATE FC_IMAGE_FIXTURE="$<TARGET_FILE:fc_fixture_no_export>")
add_dependencies(fc_control_child fc_fixture_no_export)

# Build the provider and consumer separately so patch API tests cross DLL boundaries.
add_library(fc_api_provider SHARED runtime/fixtures/patch_api_provider.cpp)
fc_configure_project_target(fc_api_provider)
target_link_libraries(fc_api_provider PRIVATE FusionCutter::SDK ws2_32)

add_library(fc_api_consumer SHARED runtime/fixtures/patch_api_consumer.cpp)
fc_configure_project_target(fc_api_consumer)
target_link_libraries(fc_api_consumer PRIVATE FusionCutter::SDK ws2_32)
target_compile_definitions(fc_tests PRIVATE
    FC_CONTROL_CHILD="$<TARGET_FILE:fc_control_child>"
    FC_API_PROVIDER="$<TARGET_FILE:fc_api_provider>"
    FC_API_CONSUMER="$<TARGET_FILE:fc_api_consumer>")
add_dependencies(fc_tests fc_control_child fc_api_provider fc_api_consumer)

# This consumer uses public SDK headers and allocates and destroys its callback captures in a separate DLL.
add_library(fc_lua_consumer SHARED patches/lua_bindings/consumer_fixture.cpp)
fc_configure_project_target(fc_lua_consumer)
target_link_libraries(fc_lua_consumer PRIVATE FusionCutter::SDK ws2_32)
target_compile_definitions(fc_tests PRIVATE FC_LUA_CONSUMER="$<TARGET_FILE:fc_lua_consumer>")
add_dependencies(fc_tests fc_lua_consumer)

fc_configure_project_target(fc_tests)
target_link_libraries(fc_tests PRIVATE fusioncutter_runtime Catch2::Catch2WithMain fc_test_lua)

# The verifier tests load this SDK plugin through the production DLL boundary.
add_library(fc_verifier_fixture SHARED verifier/plugin.cpp)
fc_configure_project_target(fc_verifier_fixture)
target_link_libraries(fc_verifier_fixture PRIVATE FusionCutter::SDK)
target_compile_definitions(fc_tests PRIVATE
    FC_VERIFIER_EXECUTABLE="$<TARGET_FILE:fusioncutter_verify>"
    FC_VERIFIER_FIXTURE="$<TARGET_FILE:fc_verifier_fixture>"
    FC_CMAKE_EXECUTABLE="${CMAKE_COMMAND}"
    FC_VERIFIER_JSON_CHECK="${CMAKE_CURRENT_SOURCE_DIR}/verifier/check_json.cmake")
add_dependencies(fc_tests fusioncutter_verify fc_verifier_fixture)
target_compile_definitions(fc_tests PRIVATE
    FC_FIXTURE_PLUGIN="$<TARGET_FILE:fc_fixture_plugin>"
    FC_FIXTURE_NO_EXPORT="$<TARGET_FILE:fc_fixture_no_export>")
target_compile_definitions(fc_tests PRIVATE
    FC_BOOTSTRAP_PROBE="$<TARGET_FILE:fc_bootstrap_probe>")
add_dependencies(fc_tests fc_fixture_plugin fc_fixture_no_export fc_bootstrap_probe)

add_library(fc_native_hook_fixture SHARED native/hook_fixture.cpp)
fc_configure_project_target(fc_native_hook_fixture)
target_compile_definitions(fc_tests PRIVATE FC_NATIVE_HOOK_FIXTURE="$<TARGET_FILE:fc_native_hook_fixture>")
add_dependencies(fc_tests fc_native_hook_fixture)

include("${FC_SOURCE_ROOT}/vendor/Catch2/extras/Catch.cmake")
catch_discover_tests(fc_tests WORKING_DIRECTORY "$<TARGET_FILE_DIR:fc_tests>")
add_test(NAME fc_loaded_image_lifetime COMMAND fc_tests "[.loaded-image]")

# Build and load the SDK example using only a relocated installation.
add_test(NAME fc_sdk_install COMMAND "${CMAKE_COMMAND}"
    "-DFC_BUILD_DIRECTORY=${CMAKE_BINARY_DIR}"
    "-DFC_CONFIGURATION=$<CONFIG>"
    "-DFC_GENERATOR=${CMAKE_GENERATOR}"
    "-DFC_MAKE_PROGRAM=${CMAKE_MAKE_PROGRAM}"
    "-DFC_TEST_EXECUTABLE=$<TARGET_FILE:fc_tests>"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/sdk_install.cmake")
set_tests_properties(fc_sdk_install PROPERTIES TIMEOUT 120)

add_subdirectory(loaders)
target_sources(fc_tests PRIVATE loaders/loader_tests.cpp)
