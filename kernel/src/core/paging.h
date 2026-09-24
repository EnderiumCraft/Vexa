#ifndef VEXA_CORE_PAGING_H
#define VEXA_CORE_PAGING_H

#include <stdint.h>

/* x86_64 page table entry bits, shared by vmm.c (kernel mappings) and vm.c
 * (user address spaces). */
#define PTE_PRESENT (1ULL << 0)
#define PTE_WRITE (1ULL << 1)
#define PTE_USER (1ULL << 2)
#define PTE_PWT (1ULL << 3)
#define PTE_PCD (1ULL << 4)
#define PTE_HUGE (1ULL << 7)     /* In a PD or PDPT entry: maps 2 MiB / 1 GiB. */
#define PTE_PAT_4K (1ULL << 7)   /* In a PT entry: third PAT index bit. */
#define PTE_PAT_2M (1ULL << 12)  /* The same bit, in a 2 MiB entry. */
#define PTE_NX (1ULL << 63)
#define PTE_ADDR_MASK 0x000ffffffffff000ULL

uint64_t vmm_kernel_pml4_phys(void);
uint64_t vmm_nx_bit(void); /* PTE_NX if the CPU supports it, else 0. */

#endif
