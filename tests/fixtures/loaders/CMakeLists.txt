add_library(fc_loader_core_stub SHARED
    core_stub.cpp
    core_stub.def
)
fc_configure_project_target(fc_loader_core_stub)
fc_set_artifact_directories(fc_loader_core_stub)
target_link_libraries(fc_loader_core_stub PRIVATE FusionCutter::SDK)

if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    add_library(fc_loader_original_stub SHARED
        original_stub.cpp
        original_stub.def
    )
    fc_configure_project_target(fc_loader_original_stub)
    fc_set_artifact_directories(fc_loader_original_stub)
else()
    add_library(fc_loader_chain_stub SHARED
        chain_stub.cpp
        chain_stub.def
    )
    fc_configure_project_target(fc_loader_chain_stub)
    fc_set_artifact_directories(fc_loader_chain_stub)
endif()
