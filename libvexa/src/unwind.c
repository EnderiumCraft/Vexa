/* Vexa has no unwinder yet, so there is no stack to walk. Rust's backtrace code and
 * the C++ runtime reference these functions; they report an empty context. */
#include <stdint.h>

uintptr_t _Unwind_GetIPInfo(void *context, int *ip_before_instruction) {
    (void)context;
    if (ip_before_instruction) {
        *ip_before_instruction = 0;
    }
    return 0;
}

uintptr_t _Unwind_GetRegionStart(void *context) {
    (void)context;
    return 0;
}

uintptr_t _Unwind_GetLanguageSpecificData(void *context) {
    (void)context;
    return 0;
}

uintptr_t _Unwind_GetDataRelBase(void *context) {
    (void)context;
    return 0;
}

uintptr_t _Unwind_GetTextRelBase(void *context) {
    (void)context;
    return 0;
}

void _Unwind_SetGR(void *context, int index, uintptr_t value) {
    (void)context;
    (void)index;
    (void)value;
}

void _Unwind_SetIP(void *context, uintptr_t value) {
    (void)context;
    (void)value;
}
