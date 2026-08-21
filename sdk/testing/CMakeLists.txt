# Public scenarios archive the complete non-mutating owner so installed consumers never link the framework runtime.
add_library(fc_testing STATIC
    testing.cpp
    $<TARGET_OBJECTS:fusioncutter_common_validation>
    $<TARGET_OBJECTS:fusioncutter_builtin_contributions>
)
add_library(FusionCutter::Testing ALIAS fc_testing)

get_property(configured_testing_bundles TARGET fusioncutter_builtin_contributions
    PROPERTY FC_CONFIGURED_BUNDLE_TARGETS)
foreach(configured_testing_bundle IN LISTS configured_testing_bundles)
    target_sources(fc_testing PRIVATE
        $<TARGET_OBJECTS:${configured_testing_bundle}>
    )
endforeach()

fc_configure_project_target(fc_testing)
fc_set_artifact_directories(fc_testing)

set_target_properties(fc_testing PROPERTIES EXPORT_NAME Testing)

target_include_directories(fc_testing PRIVATE
    "${FC_SOURCE_ROOT}/src/core"
)

target_link_libraries(fc_testing PUBLIC
    FusionCutter::SDK
)

# These archives resolve parser and instruction-decoder calls retained by the common validation objects.
target_link_libraries(fc_testing INTERFACE
    "$<BUILD_INTERFACE:FusionCutter::inih>"
    "$<BUILD_INTERFACE:Zydis>"
    "$<INSTALL_INTERFACE:FusionCutter::TestingIni>"
    "$<INSTALL_INTERFACE:FusionCutter::TestingZydis>"
    "$<INSTALL_INTERFACE:FusionCutter::TestingZycore>"
)

install(TARGETS fc_testing
    EXPORT FusionCutterSDKTargets
    ARCHIVE DESTINATION lib
)

# Compiled Testing support is self-contained at the API level but its archive retains these reviewed static calls.
install(FILES
    "$<TARGET_FILE:fc_inih>"
    "$<TARGET_FILE:Zydis>"
    "$<TARGET_FILE:Zycore>"
    "$<TARGET_FILE:Catch2>"
    "$<TARGET_FILE:Catch2WithMain>"
    DESTINATION lib/fusioncutter-testing
)

install(DIRECTORY "${FC_SOURCE_ROOT}/vendor/Catch2/src/catch2"
    DESTINATION include
    FILES_MATCHING PATTERN "*.hpp"
)

install(FILES
    "${CMAKE_BINARY_DIR}/vendor/Catch2/generated-includes/catch2/catch_user_config.hpp"
    DESTINATION include/catch2
)

install(FILES
    "${FC_SOURCE_ROOT}/vendor/Catch2/extras/Catch.cmake"
    "${FC_SOURCE_ROOT}/vendor/Catch2/extras/CatchAddTests.cmake"
    DESTINATION lib/cmake/FusionCutterSDK
)
