#include <stdbool.h>
#include <stddef.h>
#include <vexa/acpi.h>
#include <vexa/cmdline.h>
#include <vexa/kprintf.h>
#include <vexa/mm.h>
#include <vexa/string.h>

struct __attribute__((packed)) acpi_rsdp {
    char signature[8];
    uint8_t checksum;
    char oem_id[6];
    uint8_t revision;
    uint32_t rsdt_address;
    /* ACPI 2.0+ */
    uint32_t length;
    uint64_t xsdt_address;
    uint8_t extended_checksum;
    uint8_t reserved[3];
};

static const struct acpi_sdt_header *root_table; /* XSDT or RSDT. */
static bool root_is_xsdt;

static bool checksum_ok(const void *data, size_t length) {
    const uint8_t *bytes = data;
    uint8_t sum = 0;
    for (size_t i = 0; i < length; i++) {
        sum += bytes[i];
    }
    return sum == 0;
}

static const struct acpi_sdt_header *map_table(uint64_t phys) {
    const struct acpi_sdt_header *header =
        map_phys(phys, sizeof(struct acpi_sdt_header), MAP_WRITEBACK);
    return map_phys(phys, header->length, MAP_WRITEBACK);
}

static size_t root_entry_count(void) {
    return (root_table->length - sizeof(*root_table)) / (root_is_xsdt ? 8 : 4);
}

static const struct acpi_sdt_header *root_entry(size_t i) {
    /* Entries are not necessarily aligned, so copy them out. */
    size_t width = root_is_xsdt ? 8 : 4;
    uint64_t phys = 0;
    memcpy(&phys, (const uint8_t *)(root_table + 1) + i * width, width);
    return map_table(phys);
}

void acpi_init(uint64_t rsdp_phys) {
    if (cmdline_has("acpi=off")) {
        kprintf("[acpi] disabled by acpi=off\n");
        return;
    }
    if (!rsdp_phys) {
        kprintf("[acpi] no ACPI tables found\n");
        return;
    }
    const struct acpi_rsdp *rsdp = map_phys(rsdp_phys, sizeof(*rsdp), MAP_WRITEBACK);
    if (memcmp(rsdp->signature, "RSD PTR ", 8) != 0 || !checksum_ok(rsdp, 20)) {
        panic("ACPI: invalid RSDP at %p", (void *)rsdp_phys);
    }

    if (rsdp->revision >= 2 && rsdp->xsdt_address) {
        root_table = map_table(rsdp->xsdt_address);
        root_is_xsdt = true;
    } else {
        root_table = map_table(rsdp->rsdt_address);
        root_is_xsdt = false;
    }
    if (!checksum_ok(root_table, root_table->length)) {
        panic("ACPI: bad %s checksum", root_is_xsdt ? "XSDT" : "RSDT");
    }

    kprintf("[acpi] revision %u, tables:", rsdp->revision);
    for (size_t i = 0; i < root_entry_count(); i++) {
        const struct acpi_sdt_header *table = root_entry(i);
        kprintf(" %c%c%c%c", table->signature[0], table->signature[1],
                table->signature[2], table->signature[3]);
    }
    kprintf("\n");
}

const struct acpi_sdt_header *acpi_find_table(const char *signature) {
    if (!root_table) {
        return NULL;
    }
    for (size_t i = 0; i < root_entry_count(); i++) {
        const struct acpi_sdt_header *table = root_entry(i);
        if (memcmp(table->signature, signature, 4) == 0) {
            if (!checksum_ok(table, table->length)) {
                kprintf("[acpi] warning: bad checksum on %s\n", signature);
            }
            return table;
        }
    }
    return NULL;
}
