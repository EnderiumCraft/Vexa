#ifndef VEXA_ACPI_H
#define VEXA_ACPI_H

#include <stdbool.h>
#include <stdint.h>

struct __attribute__((packed)) acpi_sdt_header {
    char signature[4];
    uint32_t length;
    uint8_t revision;
    uint8_t checksum;
    char oem_id[6];
    char oem_table_id[8];
    uint32_t oem_revision;
    uint32_t creator_id;
    uint32_t creator_revision;
};

/* `rsdp_phys` is the physical address from the bootloader, or 0 if none. */
void acpi_init(uint64_t rsdp_phys);

/* Returns the first table with the given 4-letter signature, or NULL. */
const struct acpi_sdt_header *acpi_find_table(const char *signature);
/* The RSDP's physical address (0 if there's none, or ACPI is off). */
uint64_t acpi_rsdp(void);

/* The AML interpreter (uACPI, core/aml.c): loads the DSDT and SSDTs into
 * the ACPI namespace and runs their initialization. From the init thread,
 * before the drivers. */
void acpi_namespace_init(void);

/* Where a PCI device's interrupt pin (1-4: INTA-INTD) goes, by the _PRT
 * routing tables: the GSI, and how it's triggered. False if unknown. */
struct pci_device;
bool acpi_pci_route(struct pci_device *device, int pin, uint32_t *gsi, bool *level,
                    bool *active_low);

#endif
