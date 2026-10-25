#ifndef CVTEBENCH_MONITOR_BACKEND_H
#define CVTEBENCH_MONITOR_BACKEND_H

#include "monitor.h"

typedef struct CgroupScope CgroupScope;

/* Backend state is independent for each terminal launch and interval series. */
typedef struct {
    void *(*create)(pid_t terminal_pid, CgroupScope *scope);
    void (*destroy)(void *state);
    void (*set_worker)(void *state, pid_t worker_pid, pid_t terminal_pid);
    void (*begin)(void *state, uint64_t start_ns);
    void (*tick)(void *state, uint64_t now_ns);
    MonitorMetrics (*end)(void *state, uint64_t end_ns);
} MonitorBackendOps;

const MonitorBackendOps *polling_monitor_ops(void);
const MonitorBackendOps *cgroupv2_monitor_ops(void);

/* A scope is shared with the cgroup backend even when polling is selected. */
CgroupScope *cgroup_scope_create(void);
void cgroup_scope_destroy(CgroupScope *scope);
bool cgroup_scope_attach_terminal(CgroupScope *scope, pid_t terminal_pid);
const char *cgroup_scope_worker_path(const CgroupScope *scope);
bool cgroupv2_join_worker(const char *path);
MonitorIsolation cgroup_scope_check(CgroupScope *scope, pid_t terminal_pid,
                                    pid_t worker_pid, bool worker_moved,
                                    const char **detail);
const char *cgroup_scope_error(const CgroupScope *scope);

#endif
