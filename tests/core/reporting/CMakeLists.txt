fc_add_catch_test(fc_reporting_tests
    NAME fc_reporting
    LABEL fast
    SOURCES reporting_tests.cpp
    LIBRARIES
        fusioncutter_catalog
        fusioncutter_runtime
)

target_include_directories(fc_reporting_tests PRIVATE
    "${FC_SOURCE_ROOT}/src/core"
)
