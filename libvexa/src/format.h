/* libvexa's printf internals, shared with its floating-point formatting. */
#ifndef LIBVEXA_FORMAT_H
#define LIBVEXA_FORMAT_H

#include <stddef.h>
#include <stdio.h>

struct sink {
    FILE *file; /* Either a file... */
    char *out;  /* ...or a buffer of `size` bytes. */
    size_t size;
    size_t count; /* Characters produced (even past the end of the buffer). */
};

/* printf's flags, as musl spells them (fmtfp.c is musl's). */
#define ALT_FORM (1U << ('#' - ' '))
#define ZERO_PAD (1U << ('0' - ' '))
#define LEFT_ADJ (1U << ('-' - ' '))
#define PAD_POS (1U << (' ' - ' '))
#define MARK_POS (1U << ('+' - ' '))

void __libvexa_emit(struct sink *sink, char c);
/* y in conversion t ('f', 'e', 'g', 'a', or capitals), width w, precision p
 * (-1: the default), with those flags. */
int __libvexa_fmt_fp(struct sink *f, long double y, int w, int p, int fl, int t);

#endif
