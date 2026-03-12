#ifndef TIMER_H
#define TIMER_H

#include "types.h"

void timer_init(uint32_t frequency);
uint32_t timer_get_ticks(void);
uint32_t timer_get_frequency(void);

#endif /* TIMER_H */