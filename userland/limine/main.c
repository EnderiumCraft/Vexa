/* limine: Limine's own tool (limine/limine.c, BSD-2-Clause), built for Vexa:
 * `limine bios-install /dev/vda` puts Limine's BIOS boot code on a disk (the
 * installer runs it). Limine's code, as it comes; the warnings Vexa's build
 * treats as errors are its own business. */
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#pragma GCC diagnostic ignored "-Wtype-limits"
#include "../../limine/limine.c"
