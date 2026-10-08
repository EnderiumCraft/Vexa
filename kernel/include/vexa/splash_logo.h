#ifndef VEXA_SPLASH_LOGO_H
#define VEXA_SPLASH_LOGO_H

#include <stdint.h>

/* The Vexa logo, for the boot screen (made by tools/make-splash-logo.py):
 * ARGB. */
#define SPLASH_LOGO_WIDTH 288
#define SPLASH_LOGO_HEIGHT 124
extern const uint32_t splash_logo[SPLASH_LOGO_HEIGHT][SPLASH_LOGO_WIDTH];

#endif
