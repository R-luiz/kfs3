#include "gdt.h"

/* Define 7 entries (0=Null, 1=Kernel Code, 2=Kernel Data, 3=Kernel Stack, 
   4=User Code, 5=User Data, 6=User Stack) */
struct gdt_entry gdt_entries[7] __attribute__((section(".gdt")));
struct gdt_ptr gdt_ptr __attribute__((section(".gdt")));

/* Set an individual GDT entry */
static void gdt_set_gate(int num, unsigned long base, unsigned long limit, 
                         uint8_t access, uint8_t gran)
{
    gdt_entries[num].base_low    = (base & 0xFFFF);
    gdt_entries[num].base_middle = (base >> 16) & 0xFF;
    gdt_entries[num].base_high   = (base >> 24) & 0xFF;

    gdt_entries[num].limit_low   = (limit & 0xFFFF);
    gdt_entries[num].granularity = ((limit >> 16) & 0x0F);

    gdt_entries[num].granularity |= (gran & 0xF0);
    gdt_entries[num].access      = access;
}

/* Initialize and load our GDT */
void init_gdt(void)
{
    /* There are 7 entries */
    gdt_ptr.limit = (sizeof(struct gdt_entry) * 7) - 1;
    gdt_ptr.base = (unsigned int)&gdt_entries;

    /* Null segment */
    gdt_set_gate(0, 0, 0, 0, 0);

    /* Kernel Code Segment: base=0, limit=4GB, access=0x9A, granularity=0xCF */
    gdt_set_gate(1, 0, 0xFFFFFFFF, 0x9A, 0xCF);

    /* Kernel Data Segment: base=0, limit=4GB, access=0x92, granularity=0xCF */
    gdt_set_gate(2, 0, 0xFFFFFFFF, 0x92, 0xCF);

    /* Kernel Stack Segment: same as data (it is a read/write segment) */
    gdt_set_gate(3, 0, 0xFFFFFFFF, 0x92, 0xCF);

    /* User Code Segment: base=0, limit=4GB, access=0xFA, granularity=0xCF */
    gdt_set_gate(4, 0, 0xFFFFFFFF, 0xFA, 0xCF);

    /* User Data Segment: base=0, limit=4GB, access=0xF2, granularity=0xCF */
    gdt_set_gate(5, 0, 0xFFFFFFFF, 0xF2, 0xCF);

    /* User Stack Segment: same as user data */
    gdt_set_gate(6, 0, 0xFFFFFFFF, 0xF2, 0xCF);

    /* Load the new GDT using LGDT */
    asm volatile("lgdt (%0)" : : "r" (&gdt_ptr));

    /* Update segment registers:
       Here, 0x10 refers to the Kernel Data segment (index 2 * 8 = 16 = 0x10) */
    asm volatile (
        "mov $0x10, %%ax\n"
        "mov %%ax, %%ds\n"
        "mov %%ax, %%es\n"
        "mov %%ax, %%fs\n"
        "mov %%ax, %%gs\n"
        "mov %%ax, %%ss\n"
        : : : "ax"
    );

    /* Far jump to reload CS (0x08 == Kernel Code segment, index 1 * 8 = 8) */
    asm volatile (
        "ljmp $0x08, $.flush\n"
        ".flush:\n"
    );
}
