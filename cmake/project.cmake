get_filename_component(FC_SOURCE_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
set(FC_VERSION_STRING "1.0-alpha")

# Requires Windows/MSVC and sets static runtime and diagnostic defaults before targets are declared.
# Reports the compiler's x86 or x64 architecture.
function(fc_initialize_build)
    if(NOT WIN32 OR NOT MSVC)
        message(FATAL_ERROR "Fusion Cutter requires Windows and the MSVC toolchain.")
    endif()

    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>" PARENT_SCOPE)

    if(CMAKE_SIZEOF_VOID_P EQUAL 4)
        set(architecture "x86")
    elseif(CMAKE_SIZEOF_VOID_P EQUAL 8)
        set(architecture "x64")
    else()
        message(FATAL_ERROR "Fusion Cutter supports only x86 and x64 builds.")
    endif()

    message(STATUS "Fusion Cutter architecture: ${architecture}")
endfunction()

# Applies the common C++ settings and writes binaries and symbols under the build directory's out folder.
function(fc_configure_project_target target)
    target_compile_features(${target} PRIVATE cxx_std_23)
    set_target_properties(${target} PROPERTIES
        CXX_EXTENSIONS OFF
        CXX_SCAN_FOR_MODULES OFF
    )

    target_compile_definitions(${target} PRIVATE
        FC_VERSION_STRING="${FC_VERSION_STRING}"
        WIN32_LEAN_AND_MEAN
        NOMINMAX
    )

    target_compile_options(${target} PRIVATE
        /EHsc
        /permissive-
        /W4
    )

    set_target_properties(${target} PROPERTIES MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

    # RelWithDebInfo builds discard unused sections and merge identical sections while retaining debug symbols.
    target_link_options(${target} PRIVATE
        "$<$<CONFIG:RelWithDebInfo>:/INCREMENTAL:NO>"
        "$<$<CONFIG:RelWithDebInfo>:/OPT:REF>"
        "$<$<CONFIG:RelWithDebInfo>:/OPT:ICF>"
    )

    set(output_directory "${CMAKE_BINARY_DIR}/out/$<CONFIG>")
    set_target_properties(${target} PROPERTIES
        ARCHIVE_OUTPUT_DIRECTORY "${output_directory}"
        LIBRARY_OUTPUT_DIRECTORY "${output_directory}"
        PDB_OUTPUT_DIRECTORY "${output_directory}"
        RUNTIME_OUTPUT_DIRECTORY "${output_directory}"
    )
endfunction()

# Installs build information and licenses once per component.
# contains_runtime selects the hook dependency notices.
function(fc_install_delivery_info component contains_runtime)
    # Component projects can call this helper repeatedly; the first call installs that component's metadata.
    get_property(installed GLOBAL PROPERTY "FC_DELIVERY_INFO_${component}")
    if(installed)
        return()
    endif()
    set_property(GLOBAL PROPERTY "FC_DELIVERY_INFO_${component}" TRUE)

    if(CMAKE_SIZEOF_VOID_P EQUAL 4)
        set(architecture x86)
        set(coverage "BF1.Steam.5F243336, BF1.SPTest.41C75543, BF1.SPTest.41EC6058, BF1.DVD.41E48CFC, BF2.Steam.59EDE353, BF2.GOG.59EDF52B, BF2.DVD.44961FB6, BF2.ModTools.43EBD102")
    else()
        set(architecture x64)
        set(coverage "BF1.Classic.66702CCB, BF2.Classic.66702CD2; bootstrap Classic.Bootstrap.66702CD7")
    endif()

    set(windows_sdk "${CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION}")
    if(NOT windows_sdk)
        set(windows_sdk "$ENV{WindowsSDKVersion}")
        string(REGEX REPLACE "[/\\\\]+$" "" windows_sdk "${windows_sdk}")
    endif()
    if(NOT windows_sdk)
        set(windows_sdk "not reported by the configured toolchain")
    endif()

    set(info_directory "${CMAKE_CURRENT_BINARY_DIR}/delivery/${component}")
    file(GENERATE OUTPUT "${info_directory}/$<CONFIG>/BUILD_INFO.txt" CONTENT
        "Fusion Cutter ${FC_VERSION_STRING}\nArchitecture: ${architecture}\nConfiguration: $<CONFIG>\nCompiler: ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}\nWindows SDK: ${windows_sdk}\nRecognized image profiles: ${coverage}\nLive gameplay and interoperability remain unverified.\n")
    install(FILES "${info_directory}/$<CONFIG>/BUILD_INFO.txt" "${FC_SOURCE_ROOT}/LICENSE"
        DESTINATION . COMPONENT "${component}")

    if(contains_runtime)
        file(WRITE "${info_directory}/THIRD_PARTY_NOTICES.txt"
            "Fusion Cutter's runtime uses statically linked SafetyHook, Zydis and Zycore.\nTheir license texts are included under licenses. SafetyHook includes a correction for Windows allocation granularity.\nSafetyHook: 19223663fb8a573253ffb2e82da87cc354bf5c16\nZydis: 569320ad3c4856da13b9dbf1f0d9e20bda63870e\n")
        install(FILES "${info_directory}/THIRD_PARTY_NOTICES.txt" DESTINATION . COMPONENT "${component}")
        install(FILES "${FC_SOURCE_ROOT}/vendor/SafetyHook/LICENSE"
            DESTINATION licenses RENAME SafetyHook.txt COMPONENT "${component}")
        install(FILES "${FC_SOURCE_ROOT}/vendor/Zydis/LICENSE"
            DESTINATION licenses RENAME Zydis.txt COMPONENT "${component}")
        install(FILES "${FC_SOURCE_ROOT}/vendor/Zydis/dependencies/zycore/LICENSE"
            DESTINATION licenses RENAME Zycore.txt COMPONENT "${component}")
    endif()
endfunction()

# Stages a product in its delivery component and the Runtime aggregate, with separate matching symbols.
# The optional third argument selects the binary directory within the package; it defaults to the package root.
function(fc_install_binary target component)
    set(destination "${ARGV2}")
    if(NOT destination)
        set(destination ".")
    endif()

    foreach(delivery_component IN ITEMS "${component}" Runtime)
        install(TARGETS ${target} RUNTIME DESTINATION "${destination}" COMPONENT "${delivery_component}")
    endforeach()

    # Debug and RelWithDebInfo must supply their linker PDB; a missing file is a staging failure.
    foreach(symbol_component IN ITEMS Symbols Runtime)
        install(FILES "$<TARGET_PDB_FILE:${target}>" DESTINATION symbols COMPONENT "${symbol_component}"
            CONFIGURATIONS Debug RelWithDebInfo)
    endforeach()
endfunction()
