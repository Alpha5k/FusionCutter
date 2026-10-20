#pragma once

#include <fc/plugin/declarations.hpp>
#include <fc/detail/plugin_storage.hpp>

// Adds an undecorated alias for the x86 C export expected by the runtime loader.
#if defined(_M_IX86)
#define FC_PLUGIN_ENTRY __pragma(comment(linker, "/EXPORT:FC_GetPlugin=_FC_GetPlugin"))
#else
#define FC_PLUGIN_ENTRY
#endif

// Define this once per plugin DLL using a function that returns fc::Plugin; construction failures are reported to the runtime.
#define FC_PLUGIN(factory)                                                                                                                 \
    FC_PLUGIN_ENTRY                                                                                                                        \
    extern "C" FC_EXPORT std::uint32_t FC_CDECL FC_GetPlugin(std::uint32_t abi, const FC_Plugin **out, FC_ErrorSink error) {               \
        return ::fc::detail::get_plugin<&factory>(abi, out, error);                                                                        \
    }
