add_library(fusioncutter_patching STATIC
    "${FC_SOURCE_ROOT}/include/FusionCutter/patching.hpp"
    memory.cpp
    memory.hpp
    patch_plan.cpp
    patching.hpp
    plan_verification.cpp
    plan_verification.hpp
    plan_storage.hpp
    preparation.cpp
    prepared_plan.cpp
    prepared_plan.hpp
    transaction.cpp
    validation.cpp
    validation.hpp
)

target_include_directories(fusioncutter_patching PUBLIC
    "${FC_SOURCE_ROOT}/include"
    "${CMAKE_CURRENT_SOURCE_DIR}"
)

target_link_libraries(fusioncutter_patching PRIVATE
    safetyhook::safetyhook
)

fc_configure_project_target(fusioncutter_patching)
