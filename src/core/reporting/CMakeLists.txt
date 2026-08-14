add_library(fusioncutter_reporting STATIC
    "${FC_SOURCE_ROOT}/include/FusionCutter/diagnostics.hpp"
    "${FC_SOURCE_ROOT}/include/FusionCutter/reporting.hpp"
    bounded_writer.hpp
    crash_reporter.cpp
    crash_reporter.hpp
    diagnostics.cpp
    diagnostics.hpp
    log_level.hpp
    session.cpp
    session.hpp
    spsc_ring.hpp
    status.cpp
    status.hpp
)

target_include_directories(fusioncutter_reporting PUBLIC
    "${FC_SOURCE_ROOT}/include"
    "${CMAKE_CURRENT_SOURCE_DIR}"
)

target_compile_definitions(fusioncutter_reporting PRIVATE
    FC_BUILD_ID="${FC_BUILD_ID}"
    FC_VERSION_STRING="${FC_VERSION_STRING}"
)

target_link_libraries(fusioncutter_reporting PRIVATE
    quill::quill
)

fc_configure_project_target(fusioncutter_reporting)
