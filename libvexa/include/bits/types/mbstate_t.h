#ifndef LIBVEXA_BITS_TYPES_MBSTATE_T_H
#define LIBVEXA_BITS_TYPES_MBSTATE_T_H

/* mbstate_t alone (for <wchar.h>, and for C++'s library, which looks here). */
typedef struct {
    unsigned int pending; /* (UTF-8 is decoded whole: nothing is kept.) */
} mbstate_t;

#endif
