#ifndef LIBVEXA_SETJMP_H
#define LIBVEXA_SETJMP_H

#ifdef __cplusplus
extern "C" {
#endif

typedef unsigned long jmp_buf[8]; /* rbx, rbp, r12-r15, rsp, return address */
typedef jmp_buf sigjmp_buf;

int setjmp(jmp_buf env) __attribute__((returns_twice));
__attribute__((noreturn)) void longjmp(jmp_buf env, int value);
#define _setjmp setjmp
#define _longjmp longjmp
#define sigsetjmp(env, save) setjmp(env)
#define siglongjmp longjmp

#ifdef __cplusplus
}
#endif

#endif
