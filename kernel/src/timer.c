#include "timer.h"
#include "idt.h"
#include "io.h"
#include "process.h"

#define PIT_CHANNEL0_PORT     0x40
#define PIT_COMMAND_PORT      0x43
#define PIT_BASE_FREQUENCY    1193182U

static volatile uint32_t pit_ticks = 0;
static uint32_t pit_frequency = 0;

static void timer_interrupt(cpu_registers_t *registers)
{
    (void)registers;
    pit_ticks++;
    scheduler_tick();
}

void timer_init(uint32_t frequency)
{
    uint32_t divisor;

    if (frequency == 0) {
        frequency = 100;
    }

    divisor = PIT_BASE_FREQUENCY / frequency;
    if (divisor == 0) {
        divisor = 1;
    }

    pit_frequency = frequency;
    idt_register_handler(IRQ0_VECTOR, timer_interrupt);

    outb(PIT_COMMAND_PORT, 0x36);
    outb(PIT_CHANNEL0_PORT, (uint8_t)(divisor & 0xFF));
    outb(PIT_CHANNEL0_PORT, (uint8_t)((divisor >> 8) & 0xFF));

    pic_clear_mask(0);
    pic_clear_mask(2);
}

uint32_t timer_get_ticks(void)
{
    return pit_ticks;
}

uint32_t timer_get_frequency(void)
{
    return pit_frequency;
}