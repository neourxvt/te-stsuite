#ifndef TE_STSUITE_PROCESS_ISOLATION_H
#define TE_STSUITE_PROCESS_ISOLATION_H

#include <stdbool.h>
#include <stddef.h>

/* Choose one exclusive CPU and the remaining CPUs for the benchmark worker. */
bool process_isolation_choose_cpu_sets(const char *available_cpus,
                                       const char *effective_cpus,
                                       char **isolated_cpu,
                                       char **worker_cpus,
                                       char *error, size_t error_size);

#endif
