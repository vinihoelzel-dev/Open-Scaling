#ifndef TIMER_H
#define TIMER_H

#include <stdint.h>

uint64_t timer_now_ns(void);          // nanosegundos monotônicos
double   timer_elapsed_ms(uint64_t t0, uint64_t t1);

#endif