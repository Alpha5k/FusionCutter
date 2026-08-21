fc_add_catch_test(fc_runtime_tests
    NAME fc_runtime
    LABEL fast
    SOURCES
        late_image_tests.cpp
        runtime_tests.cpp
    LIBRARIES
        fusioncutter_catalog
        fusioncutter_runtime
)
