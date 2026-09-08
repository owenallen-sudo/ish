#ifndef PLATFORM_H
#define PLATFORM_H
#include "misc.h"

// Error codes for platform functions
typedef enum {
    PLATFORM_OK = 0,
    PLATFORM_ERR_READ_FAILED = 1,
    PLATFORM_ERR_INVALID_DATA = 2,
    PLATFORM_ERR_PERMISSION = 3
} platform_error_t;

// for some reason a tick is always 10ms
struct cpu_usage {
    uint64_t user_ticks;
    uint64_t system_ticks;
    uint64_t idle_ticks;
    uint64_t nice_ticks;
};

platform_error_t get_cpu_usage(struct cpu_usage *usage);

struct mem_usage {
    uint64_t total;
    uint64_t free;
    uint64_t active;
    uint64_t inactive;
};

platform_error_t get_mem_usage(struct mem_usage *usage);

struct uptime_info {
    uint64_t uptime_ticks;
    uint64_t load_1m, load_5m, load_15m;
};

platform_error_t get_uptime(struct uptime_info *info);

#endif
