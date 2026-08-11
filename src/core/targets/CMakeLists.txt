add_library(fusioncutter_targets STATIC
    layouts/aspyr.hpp
    layouts/gog_retail.hpp
    layouts/mod_tools.hpp
    layouts/profiles.cpp
    layouts/profiles.hpp
    layouts/steam_retail.hpp
    pe_image.cpp
    pe_image.hpp
    process_images.cpp
    process_images.hpp
    recognition.cpp
    recognition.hpp
)

target_include_directories(fusioncutter_targets PUBLIC
    "${FC_SOURCE_ROOT}/include"
    "${CMAKE_CURRENT_SOURCE_DIR}"
)

fc_configure_project_target(fusioncutter_targets)
