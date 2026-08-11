#include "reporting/crash_reporter.hpp"

#include <Windows.h>

BOOL WINAPI DllMain(HINSTANCE, DWORD reason, void* reserved) {
    if (reason == DLL_PROCESS_DETACH && reserved == nullptr) {
        fusioncutter::reporting::uninstall_crash_reporter();
    }
    return TRUE;
}
