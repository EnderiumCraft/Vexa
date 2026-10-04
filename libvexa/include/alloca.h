#ifndef LIBVEXA_ALLOCA_H
#define LIBVEXA_ALLOCA_H

#include <stddef.h>

/* Memory on the caller's stack, gone when it returns. */
#define alloca(size) __builtin_alloca(size)

#endif
