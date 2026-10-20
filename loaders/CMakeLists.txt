cmake_minimum_required(VERSION 3.28)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(FusionCutterLoaders LANGUAGES C CXX)
endif()

include("${CMAKE_CURRENT_LIST_DIR}/../cmake/project.cmake")
fc_initialize_build()

if(NOT TARGET FusionCutter::SDK)
    add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/../sdk" "${CMAKE_CURRENT_BINARY_DIR}/sdk")
endif()

# Common startup is compiled once per architecture and statically included in each shipping loader.
add_library(fusioncutter_loader_common STATIC
    startup.cpp
    startup.hpp
)

fc_configure_project_target(fusioncutter_loader_common)

target_include_directories(fusioncutter_loader_common PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}"
)

target_link_libraries(fusioncutter_loader_common PUBLIC
    FusionCutter::SDK
)

if(CMAKE_SIZEOF_VOID_P EQUAL 4)
    # The DirectInput proxy's .def aliases use x86 stdcall symbol names.
    add_library(FusionCutterDInput8 SHARED
        dinput8/dinput8.cpp
        dinput8/dinput8.def
    )

    fc_configure_project_target(FusionCutterDInput8)
    target_link_libraries(FusionCutterDInput8 PRIVATE fusioncutter_loader_common dinput8 dxguid)

    # Keep the .def file's COM export ordinals; /IGNORE:4222 suppresses the warning about assigning them.
    target_link_options(FusionCutterDInput8 PRIVATE /IGNORE:4222)
    set_target_properties(FusionCutterDInput8 PROPERTIES OUTPUT_NAME dinput8)

    fc_install_binary(FusionCutterDInput8 Client)
endif()

option(FC_BUILD_DEDICATED "Build the dedicated server DLL containing the loader and runtime" ON)
if(FC_BUILD_DEDICATED)
    # Dedicated servers load one DLL containing both the loader and the runtime.
    if(NOT TARGET fusioncutter_runtime)
        add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/../runtime" "${CMAKE_CURRENT_BINARY_DIR}/runtime")
    endif()

    add_library(FusionCutterRconServer SHARED
        dedicated/rconserver.cpp
        "${FC_SOURCE_ROOT}/runtime/src/core_api.cpp"
        "${FC_SOURCE_ROOT}/runtime/src/fusioncutter.def"
    )

    fc_configure_project_target(FusionCutterRconServer)
    target_link_libraries(FusionCutterRconServer PRIVATE fusioncutter_loader_common fusioncutter_runtime)

    if(CMAKE_SIZEOF_VOID_P EQUAL 4)
        set(rconserver_suffix 32)
    else()
        set(rconserver_suffix 64)
    endif()

    set_target_properties(FusionCutterRconServer PROPERTIES
        OUTPUT_NAME "RconServer_${rconserver_suffix}"
    )

    fc_install_binary(FusionCutterRconServer Dedicated)
    fc_install_delivery_info(Dedicated TRUE)
endif()

if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    # Each Classic Collection game loads its own proxy and adjacent original game DLL.
    foreach(game IN ITEMS 1 2)
        add_library(FusionCutterBattlefront${game} SHARED
            classic/entry.cpp
            classic/entry.def
        )

        fc_configure_project_target(FusionCutterBattlefront${game})
        target_compile_definitions(FusionCutterBattlefront${game} PRIVATE FC_CLASSIC_GAME_NAME="Battlefront${game}")
        target_link_libraries(FusionCutterBattlefront${game} PRIVATE fusioncutter_loader_common)
        set_target_properties(FusionCutterBattlefront${game} PROPERTIES OUTPUT_NAME Battlefront${game})

        fc_install_binary(FusionCutterBattlefront${game} Client)
    endforeach()
endif()

# Ensure loader builds include delivery metadata; the runtime adds hook dependency notices when configured.
fc_install_delivery_info(Client "${FC_BUILD_DEDICATED}")
fc_install_delivery_info(Runtime "${FC_BUILD_DEDICATED}")
fc_install_delivery_info(Symbols "${FC_BUILD_DEDICATED}")
