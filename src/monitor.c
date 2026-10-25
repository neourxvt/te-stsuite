#define _GNU_SOURCE
#include "monitor.h"
#include "monitors/backend.h"
#include "process_isolation.h"

#include <stdlib.h>
#include <string.h>

/* Keep resource accounting and process isolation behind stable APIs. */
struct Monitor {
    MonitorBackend backend;
    const MonitorBackendOps *operations;
    void *state;
    CgroupScope *scope;
    ProcessIsolation isolation;
    bool owns_isolation;
    bool isolation_ready;
    char error[512];
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

/* Construct accounting and only the cgroup scope required by the selected modes. */
Monitor *monitor_create(MonitorBackend backend,
                        ProcessIsolationMode process_isolation) {
    if (backend != MONITOR_BACKEND_POLLING &&
        backend != MONITOR_BACKEND_CGROUPV2) return NULL;
    if (process_isolation != PROCESS_ISOLATION_CGROUPV2 &&
        process_isolation != PROCESS_ISOLATION_TASKSET) return NULL;
    Monitor *monitor = calloc(1, sizeof(*monitor));
    if (monitor == NULL) return NULL;
    monitor->backend = backend;
    monitor->operations = backend == MONITOR_BACKEND_POLLING
                              ? polling_monitor_ops()
                              : cgroupv2_monitor_ops();
    bool needs_cgroup_scope = backend == MONITOR_BACKEND_CGROUPV2 ||
                              process_isolation == PROCESS_ISOLATION_CGROUPV2;
    if (needs_cgroup_scope) {
        monitor->scope = cgroup_scope_create(process_isolation);
        if (monitor->scope == NULL) {
            free(monitor);
            return NULL;
        }
    } else {
        monitor->owns_isolation = process_isolation_init(
            &monitor->isolation, process_isolation);
        if (!monitor->owns_isolation) {
            free(monitor);
            return NULL;
        }
        monitor->isolation_ready = process_isolation_select(
            &monitor->isolation, NULL, false, monitor->error,
            sizeof(monitor->error));
    }
    return monitor;
}

/* Release backend samples and whichever isolation state this mode owns. */
void monitor_destroy(Monitor *monitor) {
    if (monitor == NULL) return;
    if (monitor->operations != NULL && monitor->operations->destroy != NULL) {
        monitor->operations->destroy(monitor->state);
    }
    if (monitor->scope != NULL) {
        cgroup_scope_destroy(monitor->scope);
    }
    if (monitor->owns_isolation) {
        process_isolation_destroy(&monitor->isolation);
    }
    free(monitor);
}

/* Apply cgroup placement or taskset affinity before the terminal can exec. */
bool monitor_attach_terminal(Monitor *monitor, pid_t terminal_pid) {
    if (monitor == NULL || terminal_pid <= 0) return false;
    bool attached = false;
    if (monitor->scope != NULL) {
        attached = cgroup_scope_attach_terminal(monitor->scope, terminal_pid);
    } else if (monitor->isolation_ready) {
        attached = process_isolation_attach_terminal(
            &monitor->isolation, terminal_pid, monitor->error,
            sizeof(monitor->error));
    }
    if (!attached) return false;
    monitor->state = monitor->operations->create(terminal_pid, monitor->scope);
    return monitor->state != NULL;
}

/* Return a worker scope only for modes that use cgroups. */
const char *monitor_worker_cgroup_path(const Monitor *monitor) {
    if (monitor == NULL || monitor->scope == NULL) return "-";
    return cgroup_scope_worker_path(monitor->scope);
}

/* Expose the selected benchmark CPU for run progress output. */
const char *monitor_selected_cpu(const Monitor *monitor) {
    if (monitor == NULL) return "-";
    if (monitor->scope != NULL) {
        return cgroup_scope_selected_cpu(monitor->scope);
    }
    return monitor->isolation.isolated_cpu[0] == '\0'
               ? "-"
               : monitor->isolation.isolated_cpu;
}

/* Join a worker cgroup when configured; polling-only taskset needs none. */
bool monitor_join_worker_cgroup(const char *path) {
    if (path == NULL || strcmp(path, "-") == 0) return false;
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
    if (monitor->scope != NULL) {
        return cgroup_scope_check(monitor->scope, terminal_pid, worker_pid,
                                  worker_moved, detail);
    }
    return process_isolation_check(&monitor->isolation, NULL, terminal_pid,
                                   worker_pid, detail);
}

/* Explain setup failures without assuming that a cgroup scope exists. */
const char *monitor_scope_error(const Monitor *monitor) {
    if (monitor == NULL) return "monitor could not be created";
    if (monitor->scope != NULL) return cgroup_scope_error(monitor->scope);
    if (monitor->error[0] != '\0') return monitor->error;
    return monitor->isolation_ready ? "process isolation setup failed"
                                    : "process isolation is unavailable";
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
