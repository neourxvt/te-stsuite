#ifndef TE_STSUITE_PROCESS_ISOLATION_H
#define TE_STSUITE_PROCESS_ISOLATION_H

#include "monitor.h"

#include <stdbool.h>
#include <stddef.h>

/* Choose one exclusive CPU and the remaining CPUs for the benchmark worker. */
bool process_isolation_choose_cpu_sets(const char *available_cpus,
                                       const char *effective_cpus,
                                       char **isolated_cpu,
                                       char **worker_cpus,
                                       char *error, size_t error_size);

/* Parse the process_isolation value accepted by the root TOML table. */
bool process_isolation_parse(const char *name, ProcessIsolationMode *mode);
const char *process_isolation_name(ProcessIsolationMode mode);

/* Strategy hooks used with cgroup scopes or standalone taskset placement. */
typedef struct ProcessIsolation ProcessIsolation;
typedef struct ProcessIsolationOps ProcessIsolationOps;

struct ProcessIsolation {
    ProcessIsolationMode mode;
    const ProcessIsolationOps *ops;
    unsigned cpu;
    char isolated_cpu[32];
    char detail[512];
    char *worker_cpus;
    char *memory_nodes;
};

struct ProcessIsolationOps {
    bool (*select)(ProcessIsolation *isolation, const char *parent_path,
                   bool may_enable, char *error, size_t error_size);
    bool (*configure_base)(ProcessIsolation *isolation,
                           const char *base_path,
                           char *error, size_t error_size);
    bool (*configure_process_groups)(ProcessIsolation *isolation,
                                     const char *terminal_path,
                                     const char *worker_path,
                                     char *error, size_t error_size);
    bool (*attach_terminal)(ProcessIsolation *isolation, pid_t terminal_pid,
                            char *error, size_t error_size);
    MonitorIsolation (*check)(ProcessIsolation *isolation,
                              const char *terminal_path, pid_t terminal_pid,
                              pid_t worker_pid, const char **detail);
    void (*destroy)(ProcessIsolation *isolation);
};

/* Initialize and invoke the selected CPU placement strategy. */
bool process_isolation_init(ProcessIsolation *isolation,
                            ProcessIsolationMode mode);
bool process_isolation_select(ProcessIsolation *isolation,
                              const char *parent_path, bool may_enable,
                              char *error, size_t error_size);
bool process_isolation_configure_base(ProcessIsolation *isolation,
                                      const char *base_path,
                                      char *error, size_t error_size);
bool process_isolation_configure_process_groups(
    ProcessIsolation *isolation, const char *terminal_path,
    const char *worker_path, char *error, size_t error_size);
bool process_isolation_attach_terminal(ProcessIsolation *isolation,
                                      pid_t terminal_pid,
                                      char *error, size_t error_size);
MonitorIsolation process_isolation_check(ProcessIsolation *isolation,
                                         const char *terminal_path,
                                         pid_t terminal_pid, pid_t worker_pid,
                                         const char **detail);
void process_isolation_destroy(ProcessIsolation *isolation);

/* CPU affinity helpers used by the taskset strategy. */
bool process_isolation_pin_pid(pid_t pid, unsigned cpu);
bool process_isolation_pid_is_pinned(pid_t pid, unsigned cpu);

#endif
