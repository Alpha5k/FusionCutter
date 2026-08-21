include_guard(GLOBAL)

# Converts a qualified factory name into a validated forward declaration for generated bridge source.
function(_fc_factory_declaration factory output_variable)
    string(REPLACE "::" ";" factory_parts "${factory}")
    list(LENGTH factory_parts part_count)
    if(part_count EQUAL 0)
        message(FATAL_ERROR "A Fusion Cutter plugin FACTORY cannot be empty.")
    endif()

    foreach(part IN LISTS factory_parts)
        if(NOT part MATCHES "^[A-Za-z_][A-Za-z0-9_]*$")
            message(FATAL_ERROR "Invalid C++ identifier '${part}' in Fusion Cutter plugin FACTORY '${factory}'.")
        endif()
    endforeach()

    # Reconstruct nested namespaces explicitly so the generated bridge never depends on textual source inclusion.
    list(POP_BACK factory_parts function_name)
    set(declaration "")
    foreach(namespace_name IN LISTS factory_parts)
        string(APPEND declaration "namespace ${namespace_name} {\n")
    endforeach()
    string(APPEND declaration "fc::Plugin ${function_name}();\n")
    foreach(namespace_name IN LISTS factory_parts)
        string(APPEND declaration "}\n")
    endforeach()

    set(${output_variable} "${declaration}" PARENT_SCOPE)
endfunction()

# Builds one external plugin DLL and generates its fixed ABI query export from the named fc::Plugin factory.
function(fc_add_plugin target)
    cmake_parse_arguments(PARSE_ARGV 1 FC_PLUGIN "" "FACTORY" "SOURCES")

    if("${target}" STREQUAL "")
        message(FATAL_ERROR "fc_add_plugin() requires a target name.")
    endif()
    if(TARGET "${target}")
        message(FATAL_ERROR "fc_add_plugin(${target}) cannot replace an existing target.")
    endif()
    if(FC_PLUGIN_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR "fc_add_plugin(${target}) received unknown arguments: ${FC_PLUGIN_UNPARSED_ARGUMENTS}")
    endif()
    if(NOT FC_PLUGIN_FACTORY)
        message(FATAL_ERROR "fc_add_plugin(${target}) requires FACTORY.")
    endif()
    if(NOT FC_PLUGIN_SOURCES)
        message(FATAL_ERROR "fc_add_plugin(${target}) requires at least one source file.")
    endif()
    if(NOT TARGET FusionCutter::SDK)
        message(FATAL_ERROR "fc_add_plugin(${target}) requires the FusionCutter::SDK target.")
    endif()

    _fc_factory_declaration("${FC_PLUGIN_FACTORY}" FC_FACTORY_DECLARATION)
    set(FC_FACTORY "${FC_PLUGIN_FACTORY}")
    set(generated_directory "${CMAKE_CURRENT_BINARY_DIR}/fusioncutter-generated/${target}")
    file(MAKE_DIRECTORY "${generated_directory}")
    set(generated_bridge "${generated_directory}/plugin_bridge.cpp")
    configure_file("${CMAKE_CURRENT_FUNCTION_LIST_DIR}/FusionCutterPluginBridge.cpp.in" "${generated_bridge}" @ONLY)

    add_library(${target} MODULE
        ${FC_PLUGIN_SOURCES}
        "${generated_bridge}"
    )

    target_link_libraries(${target} PRIVATE FusionCutter::SDK)
    target_compile_features(${target} PRIVATE cxx_std_23)
    set_target_properties(${target} PROPERTIES
        CXX_EXTENSIONS OFF
        CXX_SCAN_FOR_MODULES OFF
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
        PREFIX ""
        FC_PLUGIN_POINTER_SIZE "${CMAKE_SIZEOF_VOID_P}"
    )

    if(MSVC)
        target_compile_options(${target} PRIVATE /EHsc /permissive- /W4)
        target_compile_definitions(${target} PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    else()
        message(FATAL_ERROR "Fusion Cutter plugins currently require the Visual Studio 2022 MSVC toolchain.")
    endif()
endfunction()

# Builds author tests against the compiled non-mutating scenario host and stages only explicitly named plugin DLLs.
function(fc_add_plugin_tests target)
    cmake_parse_arguments(PARSE_ARGV 1 FC_TESTS "" "" "PLUGINS;SOURCES")

    if("${target}" STREQUAL "" OR TARGET "${target}")
        message(FATAL_ERROR "fc_add_plugin_tests() requires a new target name.")
    endif()
    if(FC_TESTS_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR
            "fc_add_plugin_tests(${target}) received unknown arguments: ${FC_TESTS_UNPARSED_ARGUMENTS}"
        )
    endif()
    if(NOT FC_TESTS_PLUGINS OR NOT FC_TESTS_SOURCES)
        message(FATAL_ERROR "fc_add_plugin_tests(${target}) requires PLUGINS and SOURCES.")
    endif()
    if(NOT TARGET FusionCutter::Testing)
        message(FATAL_ERROR "fc_add_plugin_tests(${target}) requires FusionCutter::Testing.")
    endif()

    if(TARGET FusionCutter::TestingCatch2WithMain)
        set(catch_main FusionCutter::TestingCatch2WithMain)
    elseif(TARGET Catch2::Catch2WithMain)
        set(catch_main Catch2::Catch2WithMain)
    else()
        message(FATAL_ERROR "The packaged Catch2 main target is unavailable.")
    endif()

    foreach(plugin IN LISTS FC_TESTS_PLUGINS)
        if(NOT TARGET "${plugin}")
            message(FATAL_ERROR "fc_add_plugin_tests(${target}) names unknown plugin target '${plugin}'.")
        endif()
        get_target_property(plugin_pointer_size "${plugin}" FC_PLUGIN_POINTER_SIZE)
        if(NOT plugin_pointer_size OR NOT plugin_pointer_size STREQUAL "${CMAKE_SIZEOF_VOID_P}")
            message(FATAL_ERROR "Plugin target '${plugin}' does not match the test executable architecture.")
        endif()
    endforeach()

    add_executable(${target} ${FC_TESTS_SOURCES})
    target_link_libraries(${target} PRIVATE FusionCutter::Testing ${catch_main})
    target_compile_features(${target} PRIVATE cxx_std_23)
    set_target_properties(${target} PROPERTIES
        CXX_EXTENSIONS OFF
        CXX_SCAN_FOR_MODULES OFF
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
    )
    if(MSVC)
        target_compile_options(${target} PRIVATE /EHsc /permissive- /W4)
        target_compile_definitions(${target} PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    endif()

    foreach(plugin IN LISTS FC_TESTS_PLUGINS)
        add_dependencies(${target} "${plugin}")
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E make_directory "$<TARGET_FILE_DIR:${target}>/plugins"
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "$<TARGET_FILE:${plugin}>"
                "$<TARGET_FILE_DIR:${target}>/plugins/$<TARGET_FILE_NAME:${plugin}>"
            VERBATIM
        )
    endforeach()

    include(CTest)
    if(EXISTS "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/Catch.cmake")
        include("${CMAKE_CURRENT_FUNCTION_LIST_DIR}/Catch.cmake")
    elseif(DEFINED Catch2_SOURCE_DIR AND EXISTS "${Catch2_SOURCE_DIR}/extras/Catch.cmake")
        # In-tree SDK consumers use the vendored Catch module before it is copied into an installed package.
        include("${Catch2_SOURCE_DIR}/extras/Catch.cmake")
    else()
        include(Catch)
    endif()
    catch_discover_tests(${target}
        WORKING_DIRECTORY "$<TARGET_FILE_DIR:${target}>"
    )
endfunction()
