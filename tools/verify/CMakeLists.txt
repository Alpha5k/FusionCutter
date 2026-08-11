add_executable(fusioncutter_verify
    main.cpp
    mapped_image.cpp
    mapped_image.hpp
    verifier.cpp
    verifier.hpp
)

target_include_directories(fusioncutter_verify PRIVATE
    "${FC_SOURCE_ROOT}/src/core"
)

target_link_libraries(fusioncutter_verify PRIVATE
    fusioncutter_catalog
    fusioncutter_patching
    fusioncutter_reporting
    fusioncutter_targets
)

fc_configure_project_target(fusioncutter_verify)
fc_set_artifact_directories(fusioncutter_verify)

set_target_properties(fusioncutter_verify PROPERTIES
    OUTPUT_NAME FusionCutter-Verify
)
