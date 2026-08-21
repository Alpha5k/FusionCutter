fc_add_catch_test(fc_patching_tests
    NAME fc_patching
    LABEL integration
    SOURCES patching_tests.cpp
    LIBRARIES
        fusioncutter_catalog
        fusioncutter_runtime
)
