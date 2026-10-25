#ifndef CVTEBENCH_MONITOR_H
#define CVTEBENCH_MONITOR_H

#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>

/* Select the resource accounting method from the configuration file. */
typedef enum {
    MONITOR_BACKEND_POLLING,
    MONITOR_BACKEND_CGROUPV2,
} MonitorBackend;

/* Keep wall timing available even when resource counters cannot be read. */
typedef struct {
    bool resources_available;
    double wall_seconds;
    double cpu_seconds;
    double average_memory_bytes;
    uint64_t peak_memory_bytes;
} MonitorMetrics;

/* Report whether the launched terminal and worker stayed in their scopes. */
typedef enum {
    MONITOR_ISOLATION_UNAVAILABLE,
    MONITOR_ISOLATION_INSIDE,
    MONITOR_ISOLATION_OUTSIDE,
} MonitorIsolation;

typedef struct Monitor Monitor;

/* Parse and format the supported backend names used in TOML and logs. */
bool monitor_backend_parse(const char *name, MonitorBackend *backend);
const char *monitor_backend_name(MonitorBackend backend);

/* Create an accounting monitor and its private cgroup scope, when possible. */
Monitor *monitor_create(MonitorBackend backend);
void monitor_destroy(Monitor *monitor);

/* Place the terminal launcher and worker in separate cgroup children. */
bool monitor_attach_terminal(Monitor *monitor, pid_t terminal_pid);
const char *monitor_worker_cgroup_path(const Monitor *monitor);
bool monitor_join_worker_cgroup(const char *path);
MonitorIsolation monitor_check_isolation(Monitor *monitor,
                                         pid_t terminal_pid,
                                         pid_t worker_pid,
                                         bool worker_moved,
                                         const char **detail);
const char *monitor_scope_error(const Monitor *monitor);

/* Collect one startup or workload interval independently. */
void monitor_set_worker(Monitor *monitor, pid_t worker_pid,
                        pid_t terminal_pid);
void monitor_begin(Monitor *monitor, uint64_t start_ns);
void monitor_tick(Monitor *monitor, uint64_t now_ns);
MonitorMetrics monitor_end(Monitor *monitor, uint64_t end_ns);
uint64_t monotonic_now_ns(void);

#endif
