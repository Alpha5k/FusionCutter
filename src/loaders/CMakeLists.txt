# Common startup is compiled once per architecture and statically included in each shipping loader.
add_library(fusioncutter_loader_common STATIC
    common/startup.cpp
    common/startup.hpp
)

fc_configure_project_target(fusioncutter_loader_common)
fc_set_artifact_directories(fusioncutter_loader_common)

target_include_directories(fusioncutter_loader_common PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/common"
)

target_link_libraries(fusioncutter_loader_common PUBLIC
    FusionCutter::SDK
)

if(CMAKE_SIZEOF_VOID_P EQUAL 4)
    add_library(FusionCutterDInput8 SHARED
        dinput8/dinput8.cpp
        dinput8/dinput8.def
    )
    fc_configure_project_target(FusionCutterDInput8)
    fc_set_artifact_directories(FusionCutterDInput8)
    target_link_libraries(FusionCutterDInput8 PRIVATE fusioncutter_loader_common dinput8 dxguid)
    # System dinput8 compatibility requires the four standard COM names to retain their observed ordinals.
    target_link_options(FusionCutterDInput8 PRIVATE /IGNORE:4222)
    set_target_properties(FusionCutterDInput8 PROPERTIES OUTPUT_NAME dinput8)
endif()

add_library(FusionCutterRconServer SHARED
    rconserver/rconserver.cpp
)
fc_configure_project_target(FusionCutterRconServer)
fc_set_artifact_directories(FusionCutterRconServer)
target_link_libraries(FusionCutterRconServer PRIVATE fusioncutter_loader_common)
if(CMAKE_SIZEOF_VOID_P EQUAL 4)
    set(rconserver_suffix 32)
else()
    set(rconserver_suffix 64)
endif()
set_target_properties(FusionCutterRconServer PROPERTIES
    OUTPUT_NAME "RconServer_${rconserver_suffix}"
)

if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    add_library(FusionCutterBattlefront2 SHARED
        battlefront2/battlefront2.cpp
        battlefront2/battlefront2.def
    )
    fc_configure_project_target(FusionCutterBattlefront2)
    fc_set_artifact_directories(FusionCutterBattlefront2)
    target_link_libraries(FusionCutterBattlefront2 PRIVATE fusioncutter_loader_common)
    set_target_properties(FusionCutterBattlefront2 PROPERTIES OUTPUT_NAME Battlefront2)
endif()
