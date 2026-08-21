fc_add_plugin(fc_runtime_provider_slice
    FACTORY fc::fixtures::runtime::build_provider_plugin
    SOURCES provider.cpp
)

fc_add_plugin(fc_runtime_observer_slice
    FACTORY fc::fixtures::runtime::build_observer_plugin
    SOURCES observer.cpp
)

fc_add_plugin(fc_runtime_late_participant_slice
    FACTORY fc::fixtures::runtime::build_late_participant_plugin
    SOURCES late_participant.cpp
)

foreach(target IN ITEMS
        fc_runtime_provider_slice
        fc_runtime_observer_slice
        fc_runtime_late_participant_slice)
    target_include_directories(${target} PRIVATE
        "${FC_SOURCE_ROOT}/tests/fixtures/slices"
        "${CMAKE_CURRENT_SOURCE_DIR}"
    )
endforeach()
