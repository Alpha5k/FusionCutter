include_guard(GLOBAL)

if(NOT COMMAND _fc_factory_declaration)
    include("${FC_SOURCE_ROOT}/sdk/cmake/FusionCutterPlugin.cmake")
endif()

# Compiles a configured plugin factory into the framework DLL without adding another registration path.
function(fc_bundle_plugin target)
    cmake_parse_arguments(PARSE_ARGV 1 FC_BUNDLE "" "FACTORY" "SOURCES")

    if("${target}" STREQUAL "")
        message(FATAL_ERROR "fc_bundle_plugin() requires a target name.")
    endif()
    if(TARGET "${target}")
        message(FATAL_ERROR "fc_bundle_plugin(${target}) cannot replace an existing target.")
    endif()
    if(FC_BUNDLE_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR "fc_bundle_plugin(${target}) received unknown arguments: ${FC_BUNDLE_UNPARSED_ARGUMENTS}")
    endif()
    if(NOT FC_BUNDLE_FACTORY)
        message(FATAL_ERROR "fc_bundle_plugin(${target}) requires FACTORY.")
    endif()
    if(NOT FC_BUNDLE_SOURCES)
        message(FATAL_ERROR "fc_bundle_plugin(${target}) requires at least one source file.")
    endif()

    _fc_factory_declaration("${FC_BUNDLE_FACTORY}" FC_FACTORY_DECLARATION)
    string(MAKE_C_IDENTIFIER "${target}" bundle_identifier)
    string(SHA256 bundle_hash "${target};${FC_BUNDLE_FACTORY};${CMAKE_CURRENT_SOURCE_DIR}")
    string(SUBSTRING "${bundle_hash}" 0 16 bundle_hash)
    set(FC_FACTORY "${FC_BUNDLE_FACTORY}")
    set(FC_BUNDLE_REGISTER_SYMBOL "fc_bundle_register_${bundle_identifier}_${bundle_hash}")
    set(FC_BUNDLE_RELEASE_SYMBOL "fc_bundle_release_${bundle_identifier}_${bundle_hash}")
    set(generated_directory "${CMAKE_CURRENT_BINARY_DIR}/fusioncutter-generated/${target}")
    file(MAKE_DIRECTORY "${generated_directory}")
    set(generated_bridge "${generated_directory}/bundle_bridge.cpp")
    configure_file("${FC_SOURCE_ROOT}/cmake/fc_bundle_bridge.cpp.in" "${generated_bridge}" @ONLY)

    add_library(${target} OBJECT
        ${FC_BUNDLE_SOURCES}
        "${generated_bridge}"
    )
    target_link_libraries(${target} PRIVATE FusionCutter::SDK)
    fc_configure_project_target(${target})

    # Keep bundled plugin identity explicit during configuration; no static constructor participates in registration.
    set_property(GLOBAL APPEND PROPERTY FC_BUNDLE_TARGETS ${target})
    set_property(GLOBAL APPEND PROPERTY FC_BUNDLE_REGISTER_SYMBOLS ${FC_BUNDLE_REGISTER_SYMBOL})
    set_property(GLOBAL APPEND PROPERTY FC_BUNDLE_RELEASE_SYMBOLS ${FC_BUNDLE_RELEASE_SYMBOL})
endfunction()

# Materializes the bundled plugin registry and attaches every bundled plugin object to its final owning target.
function(fc_attach_configured_bundles target)
    get_property(bundle_targets GLOBAL PROPERTY FC_BUNDLE_TARGETS)
    get_property(register_symbols GLOBAL PROPERTY FC_BUNDLE_REGISTER_SYMBOLS)
    get_property(release_symbols GLOBAL PROPERTY FC_BUNDLE_RELEASE_SYMBOLS)

    # Parallel global properties preserve declaration identity while producing native declarations and table entries.
    set(declarations "")
    set(entries "")
    list(LENGTH bundle_targets bundle_count)
    if(bundle_count GREATER 0)
        math(EXPR last_bundle "${bundle_count} - 1")
        foreach(index RANGE 0 ${last_bundle})
            list(GET bundle_targets ${index} bundle_target)
            list(GET register_symbols ${index} register_symbol)
            list(GET release_symbols ${index} release_symbol)
            string(APPEND declarations
                "FC_RegisterPluginFn ${register_symbol}() noexcept;\n"
                "void ${release_symbol}() noexcept;\n")
            string(APPEND entries
                "        fc::catalog::RegistrationBridge{${register_symbol}(), &${release_symbol}},\n")
        endforeach()
    endif()

    # Generate one bridge table so bundled plugins enter the same runtime admission path as plugin DLLs.
    set_property(TARGET ${target} PROPERTY FC_CONFIGURED_BUNDLE_TARGETS "${bundle_targets}")

    set(FC_CONFIGURED_BUNDLE_DECLARATIONS "${declarations}")
    set(FC_CONFIGURED_BUNDLE_ENTRIES "${entries}")
    set(FC_CONFIGURED_BUNDLE_COUNT "${bundle_count}")
    set(generated_directory "${CMAKE_CURRENT_BINARY_DIR}/fusioncutter-generated/configured-bundles")
    file(MAKE_DIRECTORY "${generated_directory}")
    configure_file("${FC_SOURCE_ROOT}/cmake/fc_configured_bundles.cpp.in"
        "${generated_directory}/configured_bundles.cpp" @ONLY)
    target_sources(${target} PRIVATE "${generated_directory}/configured_bundles.cpp")
endfunction()
