#define _GNU_SOURCE
#include "process_isolation.h"

#include <errno.h>
#include <sched.h>

/* Keep affinity setup out of the controller's child-launch sequence. */
bool process_isolation_pin_cpu(unsigned cpu) {
    if (cpu >= CPU_SETSIZE) {
        errno = EINVAL;
        return false;
    }

    cpu_set_t cpus;
    CPU_ZERO(&cpus);
    CPU_SET(cpu, &cpus);
    return sched_setaffinity(0, sizeof(cpus), &cpus) == 0;
}
