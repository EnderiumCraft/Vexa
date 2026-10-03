/* (No include guard: <assert.h> follows NDEBUG each time it's included.) */
#undef assert
#ifdef NDEBUG
#define assert(x) ((void)0)
#else
__attribute__((noreturn)) void __assert_fail(const char *expression, const char *file, int line,
                                             const char *function);
#define assert(x) ((x) ? (void)0 : __assert_fail(#x, __FILE__, __LINE__, __func__))
#endif
#ifndef static_assert
#define static_assert _Static_assert
#endif
