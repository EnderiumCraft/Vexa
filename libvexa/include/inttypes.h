#ifndef LIBVEXA_INTTYPES_H
#define LIBVEXA_INTTYPES_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PRId8 "d"
#define PRId16 "d"
#define PRId32 "d"
#define PRId64 "ld"
#define PRIi8 "i"
#define PRIi16 "i"
#define PRIi32 "i"
#define PRIi64 "li"
#define PRIu8 "u"
#define PRIu16 "u"
#define PRIu32 "u"
#define PRIu64 "lu"
#define PRIx8 "x"
#define PRIx16 "x"
#define PRIx32 "x"
#define PRIx64 "lx"
#define PRIX8 "X"
#define PRIX16 "X"
#define PRIX32 "X"
#define PRIX64 "lX"
#define PRIo32 "o"
#define PRIo64 "lo"
#define PRIdPTR "ld"
#define PRIuPTR "lu"
#define PRIxPTR "lx"
#define PRIdMAX "ld"
#define PRIiMAX "li"
#define PRIuMAX "lu"
#define PRIxMAX "lx"
#define PRIXMAX "lX"
#define PRIoMAX "lo"
#define PRIiPTR "li"
#define PRIXPTR "lX"
#define PRIoPTR "lo"
#define PRIo8 "o"
#define PRIo16 "o"
#define SCNd8 "hhd"
#define SCNd16 "hd"
#define SCNi32 "i"
#define SCNi64 "li"
#define SCNu8 "hhu"
#define SCNu16 "hu"
#define SCNx8 "hhx"
#define SCNx16 "hx"
#define SCNdMAX "ld"
#define SCNuMAX "lu"
#define SCNxMAX "lx"
#define SCNd32 "d"
#define SCNd64 "ld"
#define SCNu32 "u"
#define SCNu64 "lu"
#define SCNx32 "x"
#define SCNx64 "lx"

intmax_t strtoimax(const char *text, char **end, int base);
uintmax_t strtoumax(const char *text, char **end, int base);

#ifdef __cplusplus
}
#endif

#endif
