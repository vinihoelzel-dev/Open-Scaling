#include "timer.h"
#include <time.h>

uint64_t timer_now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

double timer_elapsed_ms(uint64_t t0, uint64_t t1) {
    return (double)(t1 - t0) / 1e6;
}