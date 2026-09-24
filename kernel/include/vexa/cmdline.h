#ifndef VEXA_CMDLINE_H
#define VEXA_CMDLINE_H

#include <stdbool.h>

/* Kernel command line options, set in limine.conf (`cmdline:`). Recognized:
 *   acpi=off   ignore the ACPI tables
 *   noapic     use the legacy 8259 PIC and PIT instead of the APICs
 *   nosmp      use only the first CPU */
void cmdline_init(const char *cmdline);
const char *cmdline_get(void);
/* True if `option` appears as a whole space-separated word. */
bool cmdline_has(const char *option);

#endif
