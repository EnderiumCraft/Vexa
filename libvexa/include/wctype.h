#ifndef LIBVEXA_WCTYPE_H
#define LIBVEXA_WCTYPE_H

#include <wchar.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Classes and case of wide characters (code points): musl's, with
 * Unicode's tables. */
typedef unsigned long wctype_t;
typedef const int *wctrans_t;

int iswalnum(wint_t c);
int iswalpha(wint_t c);
int iswblank(wint_t c);
int iswcntrl(wint_t c);
int iswdigit(wint_t c);
int iswgraph(wint_t c);
int iswlower(wint_t c);
int iswprint(wint_t c);
int iswpunct(wint_t c);
int iswspace(wint_t c);
int iswupper(wint_t c);
int iswxdigit(wint_t c);
int iswctype(wint_t c, wctype_t type);
wctype_t wctype(const char *name);
wint_t towlower(wint_t c);
wint_t towupper(wint_t c);
wint_t towctrans(wint_t c, wctrans_t how);
wctrans_t wctrans(const char *name);

#ifdef __cplusplus
}
#endif

#endif
