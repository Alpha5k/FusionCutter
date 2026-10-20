#include <fc/abi.h>

// A different export makes this a valid DLL for exercising the missing FC_GetPlugin error.
FC_EXPORT int FC_CDECL NotAPlugin(void) {
    return 1;
}
