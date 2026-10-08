#ifndef VEXA_VERSION_H
#define VEXA_VERSION_H

/* Vexa's version is 0.MINOR.BUILD:
 *   MINOR is for big releases. Change it here, in the commit that makes the
 *     release (CI then publishes a v0.MINOR release page for it).
 *   BUILD counts the builds of a MINOR: every nightly build is one (CI sets
 *     VEXA_BUILD from its run number, see .github/workflows/build.yml). A local
 *     build is 0 unless VEXA_BUILD is given (make VEXA_BUILD=7).
 */
#define VEXA_MINOR 31

#ifndef VEXA_BUILD
#define VEXA_BUILD 0
#endif

#define VEXA_STRING2(x) #x
#define VEXA_STRING(x) VEXA_STRING2(x)
#define VEXA_VERSION "0." VEXA_STRING(VEXA_MINOR) "." VEXA_STRING(VEXA_BUILD)

#endif
