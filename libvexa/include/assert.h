/* (No include guard: <assert.h> follows NDEBUG each time it's included.) */
#undef assert
#ifdef NDEBUG
#define assert(x) ((void)0)
#else
#ifdef __cplusplus
extern "C"
#endif
__attribute__((noreturn)) void __assert_fail(const char *expression, const char *file, int line,
                                             const char *function);
#define assert(x) ((x) ? (void)0 : __assert_fail(#x, __FILE__, __LINE__, __func__))
#endif
#if !defined(static_assert) && !defined(__cplusplus)
#define static_assert _Static_assert
#endif
