add_library(fusioncutter_config STATIC
    "${FC_SOURCE_ROOT}/include/FusionCutter/templates/settings.hpp"
    "${FC_SOURCE_ROOT}/include/FusionCutter/settings.hpp"
    configuration.cpp
    configuration.hpp
    settings.cpp
)

target_include_directories(fusioncutter_config PUBLIC
    "${FC_SOURCE_ROOT}/include"
    "${CMAKE_CURRENT_SOURCE_DIR}"
)

target_link_libraries(fusioncutter_config PRIVATE
    FusionCutter::inih
)

fc_configure_project_target(fusioncutter_config)
