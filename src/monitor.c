#define _GNU_SOURCE
#include "monitor.h"
#include "monitors/backend.h"

#include <stdlib.h>
#include <string.h>

/* Keep backend selection and cgroup placement behind one stable API. */
struct Monitor {
    MonitorBackend backend;
    const MonitorBackendOps *operations;
    void *state;
    CgroupScope *scope;
};

/* Accept only the backend names users can place in the TOML root table. */
bool monitor_backend_parse(const char *name, MonitorBackend *backend) {
    if (name == NULL || backend == NULL) return false;
    if (strcmp(name, "polling") == 0) {
        *backend = MONITOR_BACKEND_POLLING;
        return true;
    }
    if (strcmp(name, "cgroupv2") == 0) {
        *backend = MONITOR_BACKEND_CGROUPV2;
        return true;
    }
    return false;
}

/* Return the canonical configuration spelling for a monitor backend. */
const char *monitor_backend_name(MonitorBackend backend) {
    switch (backend) {
        case MONITOR_BACKEND_POLLING: return "polling";
        case MONITOR_BACKEND_CGROUPV2: return "cgroupv2";
    }
    return "unknown";
}

/* Construct the shared monitor and keep cgroup isolation available to both backends. */
Monitor *monitor_create(MonitorBackend backend) {
    if (backend != MONITOR_BACKEND_POLLING &&
        backend != MONITOR_BACKEND_CGROUPV2) return NULL;
    Monitor *monitor = calloc(1, sizeof(*monitor));
    if (monitor == NULL) return NULL;
    monitor->backend = backend;
    monitor->operations = backend == MONITOR_BACKEND_POLLING
                              ? polling_monitor_ops()
                              : cgroupv2_monitor_ops();
    monitor->scope = cgroup_scope_create();
    if (monitor->scope == NULL) {
        free(monitor);
        return NULL;
    }
    return monitor;
}

/* Release backend samples before removing the terminal's private cgroup. */
void monitor_destroy(Monitor *monitor) {
    if (monitor == NULL) return;
    if (monitor->operations != NULL && monitor->operations->destroy != NULL) {
        monitor->operations->destroy(monitor->state);
    }
    cgroup_scope_destroy(monitor->scope);
    free(monitor);
}

/* Attach before exec so all terminal descendants inherit the isolated cpuset. */
bool monitor_attach_terminal(Monitor *monitor, pid_t terminal_pid) {
    if (monitor == NULL || terminal_pid <= 0) return false;
    bool attached = cgroup_scope_attach_terminal(monitor->scope, terminal_pid);
    monitor->state = monitor->operations->create(terminal_pid, monitor->scope);
    return attached && monitor->state != NULL;
}

/* Give the in-terminal worker a sibling scope that excludes its own activity. */
const char *monitor_worker_cgroup_path(const Monitor *monitor) {
    if (monitor == NULL) return "-";
    return cgroup_scope_worker_path(monitor->scope);
}

/* Expose the automatically selected isolated CPU for run progress output. */
const char *monitor_isolated_cpu(const Monitor *monitor) {
    if (monitor == NULL) return "-";
    return cgroup_scope_isolated_cpu(monitor->scope);
}

/* Join the worker's current process to the private sibling cgroup. */
bool monitor_join_worker_cgroup(const char *path) {
    return cgroupv2_join_worker(path);
}

/* Check the terminal parent and worker placement reported by the worker. */
MonitorIsolation monitor_check_isolation(Monitor *monitor,
                                         pid_t terminal_pid,
                                         pid_t worker_pid,
                                         bool worker_moved,
                                         const char **detail) {
    if (monitor == NULL) {
        if (detail != NULL) *detail = "monitor could not be created";
        return MONITOR_ISOLATION_UNAVAILABLE;
    }
    return cgroup_scope_check(monitor->scope, terminal_pid, worker_pid,
                              worker_moved, detail);
}

/* Explain a missing cgroup delegation without fabricating resource counters. */
const char *monitor_scope_error(const Monitor *monitor) {
    return monitor == NULL ? "monitor could not be created"
                           : cgroup_scope_error(monitor->scope);
}

/* Keep the worker and terminal PIDs in process-tree backend state. */
void monitor_set_worker(Monitor *monitor, pid_t worker_pid,
                        pid_t terminal_pid) {
    if (monitor == NULL || monitor->state == NULL) return;
    monitor->operations->set_worker(monitor->state, worker_pid, terminal_pid);
}

/* Start a fresh independent interval for startup or one workload. */
void monitor_begin(Monitor *monitor, uint64_t start_ns) {
    if (monitor == NULL || monitor->state == NULL) return;
    monitor->operations->begin(monitor->state, start_ns);
}

/* Refresh sampled counters while the controller waits for worker events. */
void monitor_tick(Monitor *monitor, uint64_t now_ns) {
    if (monitor == NULL || monitor->state == NULL) return;
    monitor->operations->tick(monitor->state, now_ns);
}

/* End one interval and preserve wall time even if resource accounting failed. */
MonitorMetrics monitor_end(Monitor *monitor, uint64_t end_ns) {
    if (monitor == NULL || monitor->state == NULL) return (MonitorMetrics){0};
    return monitor->operations->end(monitor->state, end_ns);
}
