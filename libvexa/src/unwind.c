/* Vexa has no unwinder yet, so there is no stack to walk. Rust's backtrace code and
 * the C++ runtime reference these functions; they report an empty context, and
 * _Unwind_Backtrace ends at once with _URC_END_OF_STACK. */
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

int _Unwind_Backtrace(int (*trace)(void *context, void *arg), void *arg) {
    (void)trace;
    (void)arg;
    return 5; /* _URC_END_OF_STACK */
}

uintptr_t _Unwind_GetIP(void *context) {
    (void)context;
    return 0;
}

uintptr_t _Unwind_GetCFA(void *context) {
    (void)context;
    return 0;
}

void *_Unwind_FindEnclosingFunction(void *pc) {
    (void)pc;
    return 0;
}
