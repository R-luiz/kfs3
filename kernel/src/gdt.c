#include "gdt.h"

#define GDT_STRINGIFY_IMPL(value) #value
#define GDT_STRINGIFY(value) GDT_STRINGIFY_IMPL(value)

/* Build the GDT in normal storage, then copy it to 0x800 before loading it. */
static struct gdt_entry gdt_entries[GDT_ENTRY_COUNT];
static struct gdt_ptr gdt_descriptor;

static void gdt_copy_to_low_memory(void)
{
    uint8_t *dst = (uint8_t*)GDT_BASE_ADDRESS;
    const uint8_t *src = (const uint8_t*)gdt_entries;
    size_t count = sizeof(gdt_entries);

    asm volatile("cld; rep movsb"
                 : "+D"(dst), "+S"(src), "+c"(count)
                 :
                 : "memory");
}

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
    gdt_descriptor.limit = (sizeof(struct gdt_entry) * GDT_ENTRY_COUNT) - 1;
    gdt_descriptor.base = GDT_BASE_ADDRESS;

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

    gdt_copy_to_low_memory();

    /* Load the new GDT using LGDT */
    asm volatile("lgdt (%0)" : : "r" (&gdt_descriptor));

    /* Load the flat data selector into the data registers and the stack selector into SS. */
    asm volatile (
        "mov $" GDT_STRINGIFY(GDT_KERNEL_DATA_SELECTOR) ", %%ax\n"
        "mov %%ax, %%ds\n"
        "mov %%ax, %%es\n"
        "mov %%ax, %%fs\n"
        "mov %%ax, %%gs\n"
        "mov $" GDT_STRINGIFY(GDT_KERNEL_STACK_SELECTOR) ", %%ax\n"
        "mov %%ax, %%ss\n"
        : : : "ax"
    );

    /* Far jump to reload CS with the kernel code selector. */
    asm volatile (
        "ljmp $" GDT_STRINGIFY(GDT_KERNEL_CODE_SELECTOR) ", $.flush\n"
        ".flush:\n"
    );
}
