#ifndef VEXA_MEMTEST_H
#define VEXA_MEMTEST_H

#include <stdbool.h>
#include <stdint.h>

/* Allocates and frees `operations` random-sized heap objects and page blocks,
 * checking contents are intact and that every page comes back. Prints a report. */
bool memtest_run(uint64_t operations);

#endif
