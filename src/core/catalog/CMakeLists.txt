set(generated_catalog "${CMAKE_CURRENT_BINARY_DIR}/generated/patch_catalog.cpp")
fc_generate_patch_catalog(
    PATCH_ROOT "${FC_SOURCE_ROOT}/src/patches"
    OUTPUT "${generated_catalog}"
    OUT_SOURCES patch_sources
)

add_library(fusioncutter_catalog STATIC
    "${FC_SOURCE_ROOT}/include/FusionCutter/categories.hpp"
    "${FC_SOURCE_ROOT}/include/FusionCutter/outcome.hpp"
    "${FC_SOURCE_ROOT}/include/FusionCutter/patch.hpp"
    "${FC_SOURCE_ROOT}/include/FusionCutter/templates/patch.hpp"
    catalog.cpp
    catalog.hpp
    generated_catalog.hpp
    "${generated_catalog}"
    ${patch_sources}
)

target_include_directories(fusioncutter_catalog PUBLIC
    "${FC_SOURCE_ROOT}/include"
    "${CMAKE_CURRENT_SOURCE_DIR}"
)

target_include_directories(fusioncutter_catalog PRIVATE
    "${FC_SOURCE_ROOT}/src/patches"
)

target_link_libraries(fusioncutter_catalog PUBLIC
    fusioncutter_config
)

if(FC_ARCHITECTURE STREQUAL "x86")
    target_link_libraries(fusioncutter_catalog PUBLIC Bcrypt Ws2_32)
endif()

fc_configure_project_target(fusioncutter_catalog)
target_compile_definitions(fusioncutter_catalog PRIVATE FC_PATCH_BUILD_ROLE_MASK=${FC_CORE_ROLE_MASK})
