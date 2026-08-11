add_subdirectory(config)
add_subdirectory(catalog)
add_subdirectory(patching)
add_subdirectory(reporting)
add_subdirectory(targets)

add_library(fusioncutter_startup STATIC
    startup.cpp
    startup.hpp
)

target_include_directories(fusioncutter_startup PUBLIC
    "${FC_SOURCE_ROOT}/include"
    "${CMAKE_CURRENT_SOURCE_DIR}"
)

target_link_libraries(fusioncutter_startup PUBLIC
    fusioncutter_catalog
    fusioncutter_patching
)

fc_configure_project_target(fusioncutter_startup)

add_library(fusioncutter_core SHARED
    "${FC_SOURCE_ROOT}/include/FusionCutter/target.hpp"
    core_api.cpp
    dllmain.cpp
    fusioncutter.def
)

target_include_directories(fusioncutter_core PUBLIC
    "${FC_SOURCE_ROOT}/include"
)

# Build and link the reviewed backends without exposing their headers or compile definitions to general core and patch
# sources. Their owning adapters carry the corresponding usage requirements.
target_link_libraries(fusioncutter_core PRIVATE
    fusioncutter_reporting
    fusioncutter_startup
    fusioncutter_targets
)

fc_configure_project_target(fusioncutter_core)
fc_configure_core_role(fusioncutter_core)
fc_set_artifact_directories(fusioncutter_core)

set_target_properties(fusioncutter_core PROPERTIES
    OUTPUT_NAME FusionCutter
)
