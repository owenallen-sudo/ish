name=darwin.c
#include <math.h>         /* for llround() */
#include <mach/mach.h>
#include <mach/mach_host.h>
#include <sys/sysctl.h>
#include <sys/resource.h>
#include <sys/time.h>
#include <stdint.h>
#include <string.h>
#include "platform/platform.h"

#ifndef DARWIN_C
#define DARWIN_C
#ifdef __cplusplus
extern "C" {
#endif

#ifndef FSHIFT
/* FSHIFT is unused in this file but preserved in case other code depends on it. */
#define FSHIFT 8
#endif

/* Fixed-point scale applied to reported load averages: a value of 123
 * means a load average of 1.23. Consumers must divide by this to get
 * the real value. */
#define LOAD_AVG_REPORT_SCALE 100

struct cpu_usage get_cpu_usage(void) {
    host_cpu_load_info_data_t load = {0};
    mach_msg_type_number_t count = HOST_CPU_LOAD_INFO_COUNT;
    struct cpu_usage usage = {0};

    host_t host = mach_host_self();
    if (host == MACH_PORT_NULL) {
        usage.valid = 0;
        return usage;
    }

    kern_return_t status = host_statistics(host,
                                          HOST_CPU_LOAD_INFO,
                                          (host_info_t)&load,
                                          &count);
    if (status != KERN_SUCCESS) {
        /* Avoid leaking the host port */
        mach_port_deallocate(mach_task_self(), host);
        usage.valid = 0;
        return usage;
    }

    usage.user_ticks   = load.cpu_ticks[CPU_STATE_USER];
    usage.system_ticks = load.cpu_ticks[CPU_STATE_SYSTEM];
    usage.idle_ticks   = load.cpu_ticks[CPU_STATE_IDLE];
    usage.nice_ticks   = load.cpu_ticks[CPU_STATE_NICE];
    usage.valid = 1;

    /* Deallocate the host port before returning */
    mach_port_deallocate(mach_task_self(), host);
    return usage;
}

/* get_mem_usage – helper to fetch page size robustly */
static int get_page_size(int64_t *out_page_size) {
    if (out_page_size == NULL) return -1;
    int64_t ps64 = 0;
    size_t sz = sizeof(ps64);
    if (sysctlbyname("hw.pagesize", &ps64, &sz, NULL, 0) != 0 || ps64 <= 0) {
        return -1;
    }
    *out_page_size = ps64;
    return 0;
}

static int get_total_memory(uint64_t *out_total) {
    if (out_total == NULL) return -1;
    uint64_t mem = 0;
    size_t sz = sizeof(mem);
    if (sysctlbyname("hw.memsize", &mem, &sz, NULL, 0) != 0 || mem == 0) {
        return -1;
    }
    *out_total = mem;
    return 0;
}

struct mem_usage get_mem_usage(void) {
    struct mem_usage usage = {0};

    vm_statistics64_data_t vm = {0};
    mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;

    host_t host = mach_host_self();
    if (host == MACH_PORT_NULL) {
        usage.valid = 0;
        return usage;
    }

    kern_return_t status = host_statistics64(host,
                                            HOST_VM_INFO64,
                                            (host_info_t)&vm,
                                            &count);
    if (status != KERN_SUCCESS) {
        mach_port_deallocate(mach_task_self(), host);
        usage.valid = 0;
        return usage;
    }

    int64_t pageSize = 0;
    if (get_page_size(&pageSize) != 0) {
        mach_port_deallocate(mach_task_self(), host);
        usage.valid = 0;
        return usage;
    }

    uint64_t total = 0;
    if (get_total_memory(&total) != 0) {
        mach_port_deallocate(mach_task_self(), host);
        usage.valid = 0;
        return usage;
    }

    usage.total    = total;
    usage.free     = (uint64_t)vm.free_count     * (uint64_t)pageSize;
    usage.active   = (uint64_t)vm.active_count   * (uint64_t)pageSize;
    usage.inactive = (uint64_t)vm.inactive_count * (uint64_t)pageSize;
    usage.wired    = (uint64_t)vm.wire_count     * (uint64_t)pageSize;
    usage.valid    = 1;

    /* Deallocate host port now that we've finished using it */
    mach_port_deallocate(mach_task_self(), host);
    return usage;
}

/* get_uptime – proper timeval handling and correctly-scaled load averages */
struct uptime_info get_uptime(void) {
    struct uptime_info uptime = {0};

    struct timeval boottime = {0};
    size_t sz = sizeof(boottime);
    if (sysctlbyname("kern.boottime", &boottime, &sz, NULL, 0) != 0) {
        uptime.valid = 0;
        return uptime;
    }

    struct timeval now;
    if (gettimeofday(&now, NULL) != 0) {
        uptime.valid = 0;
        return uptime;
    }

    /* Guard against clock skew / adjustment making boottime look later than now */
    if (now.tv_sec < boottime.tv_sec) {
        uptime.valid = 0;
        return uptime;
    }

    struct loadavg vm_loadavg = {0};
    sz = sizeof(vm_loadavg);
    if (sysctlbyname("vm.loadavg", &vm_loadavg, &sz, NULL, 0) != 0
        || vm_loadavg.fscale <= 0) {
        uptime.valid = 0;
        return uptime;
    }

    double scale = (double)vm_loadavg.fscale;
    uptime.uptime_ticks = (uint64_t)(now.tv_sec - boottime.tv_sec);
    /* llround returns long long; cast to uint64_t for the struct fields */
    uptime.load_1m  = (uint64_t)llround((vm_loadavg.ldavg[0] / scale) * LOAD_AVG_REPORT_SCALE);
    uptime.load_5m  = (uint64_t)llround((vm_loadavg.ldavg[1] / scale) * LOAD_AVG_REPORT_SCALE);
    uptime.load_15m = (uint64_t)llround((vm_loadavg.ldavg[2] / scale) * LOAD_AVG_REPORT_SCALE);
    uptime.valid = 1;
    return uptime;
}

#ifdef __cplusplus
}
#endif
#endif /* DARWIN_C */
