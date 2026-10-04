#ifndef LIBVEXA_ICONV_H
#define LIBVEXA_ICONV_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Character set conversion (musl's iconv): UTF-8, UTF-16 and UTF-32 (with
 * byte orders), ISO-8859-*, the Windows and DOS code pages, KOI8, and the
 * Japanese, Chinese and Korean multi-byte sets. */
typedef void *iconv_t;

iconv_t iconv_open(const char *to, const char *from);
size_t iconv(iconv_t cd, char **in, size_t *in_left, char **out, size_t *out_left);
int iconv_close(iconv_t cd);

#ifdef __cplusplus
}
#endif

#endif
