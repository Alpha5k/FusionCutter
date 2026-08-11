if(FC_ARCHITECTURE STREQUAL "x86")
    add_subdirectory(dinput8)
endif()

if(FC_ARCHITECTURE STREQUAL "x64")
    add_subdirectory(battlefront2)
endif()

add_subdirectory(rconserver)
