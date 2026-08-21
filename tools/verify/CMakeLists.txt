add_executable(fusioncutter_verify
    main.cpp
)

fc_configure_project_target(fusioncutter_verify)
fc_set_artifact_directories(fusioncutter_verify)

target_include_directories(fusioncutter_verify PRIVATE
    "${FC_SOURCE_ROOT}/src/core"
)

target_link_libraries(fusioncutter_verify PRIVATE
    FusionCutter::Testing
)

set_target_properties(fusioncutter_verify PROPERTIES
    OUTPUT_NAME FusionCutterVerify
)
