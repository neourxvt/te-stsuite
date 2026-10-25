#define _GNU_SOURCE
#include "backend.h"

#include <errno.h>
#include <sched.h>
#include <stdio.h>
#include <string.h>

/* Validate that CPU zero is in this process's allowed scheduler mask. */
static bool taskset_select(ProcessIsolation *isolation,
                           const char *parent_path, bool may_enable,
                           char *error, size_t error_size) {
    (void)parent_path;
    (void)may_enable;
    cpu_set_t allowed;
    CPU_ZERO(&allowed);
    if (sched_getaffinity(0, sizeof(allowed), &allowed) != 0) {
        snprintf(error, error_size, "cannot inspect CPU affinity: %s",
                 strerror(errno));
        return false;
    }
    if (!CPU_ISSET(0, &allowed)) {
        snprintf(error, error_size,
                 "CPU 0 is not available for taskset process isolation");
        return false;
    }
    isolation->cpu = 0;
    snprintf(isolation->isolated_cpu, sizeof(isolation->isolated_cpu), "0");
    return true;
}

/* Affinity placement leaves the scope's cgroup configuration untouched. */
static bool taskset_configure_base(ProcessIsolation *isolation,
                                   const char *base_path,
                                   char *error, size_t error_size) {
    (void)isolation;
    (void)base_path;
    (void)error;
    (void)error_size;
    return true;
}

/* Affinity placement does not configure terminal or worker cgroup CPU sets. */
static bool taskset_configure_process_groups(ProcessIsolation *isolation,
                                             const char *terminal_path,
                                             const char *worker_path,
                                             char *error,
                                             size_t error_size) {
    (void)isolation;
    (void)terminal_path;
    (void)worker_path;
    (void)error;
    (void)error_size;
    return true;
}

/* Pin the terminal before its launch gate opens and descendants are created. */
static bool taskset_attach_terminal(ProcessIsolation *isolation,
                                    pid_t terminal_pid,
                                    char *error, size_t error_size) {
    if (process_isolation_pin_pid(terminal_pid, isolation->cpu)) return true;
    snprintf(error, error_size, "cannot pin terminal to CPU 0: %s",
             strerror(errno));
    return false;
}

/* Verify that terminal and worker processes retained CPU 0 affinity. */
static MonitorIsolation taskset_check(ProcessIsolation *isolation,
                                      const char *terminal_path,
                                      pid_t terminal_pid, pid_t worker_pid,
                                      const char **detail) {
    (void)terminal_path;
    if (!process_isolation_pid_is_pinned(terminal_pid, isolation->cpu)) {
        snprintf(isolation->detail, sizeof(isolation->detail),
                 "terminal is not pinned to CPU 0");
        if (detail != NULL) *detail = isolation->detail;
        return MONITOR_ISOLATION_OUTSIDE;
    }
    if (!process_isolation_pid_is_pinned(worker_pid, isolation->cpu)) {
        snprintf(isolation->detail, sizeof(isolation->detail),
                 "worker is not pinned to CPU 0");
        if (detail != NULL) *detail = isolation->detail;
        return MONITOR_ISOLATION_OUTSIDE;
    }
    snprintf(isolation->detail, sizeof(isolation->detail),
             "terminal and worker are pinned to CPU 0 by affinity");
    if (detail != NULL) *detail = isolation->detail;
    return MONITOR_ISOLATION_INSIDE;
}

/* Affinity state has no dynamic resources to release. */
static void taskset_destroy(ProcessIsolation *isolation) {
    (void)isolation;
}

const ProcessIsolationOps *taskset_process_isolation_ops(void) {
    static const ProcessIsolationOps operations = {
        .select = taskset_select,
        .configure_base = taskset_configure_base,
        .configure_process_groups = taskset_configure_process_groups,
        .attach_terminal = taskset_attach_terminal,
        .check = taskset_check,
        .destroy = taskset_destroy,
    };
    return &operations;
}
