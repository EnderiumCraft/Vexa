#ifndef LIBVEXA_LOCALE_H
#define LIBVEXA_LOCALE_H

#ifdef __cplusplus
extern "C" {
#endif

/* One locale: "C" (with UTF-8 text). */
#define LC_CTYPE 0
#define LC_NUMERIC 1
#define LC_TIME 2
#define LC_COLLATE 3
#define LC_MONETARY 4
#define LC_MESSAGES 5
#define LC_ALL 6

struct lconv {
    char *decimal_point, *thousands_sep, *grouping;
    char *int_curr_symbol, *currency_symbol, *mon_decimal_point, *mon_thousands_sep;
    char *mon_grouping, *positive_sign, *negative_sign;
    char int_frac_digits, frac_digits, p_cs_precedes, p_sep_by_space, n_cs_precedes;
    char n_sep_by_space, p_sign_posn, n_sign_posn;
};

char *setlocale(int category, const char *locale);
struct lconv *localeconv(void);

#ifdef __cplusplus
}
#endif

#endif
