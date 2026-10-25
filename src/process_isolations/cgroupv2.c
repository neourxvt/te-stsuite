#define _GNU_SOURCE
#include "backend.h"
#include "../cgroupv2_internal.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Read one cgroup control file and return its trimmed contents. */
static char *read_control_value(const char *cgroup_path, const char *name) {
    char *path = cgroupv2_child_path(cgroup_path, name);
    if (path == NULL) {
        errno = ENOMEM;
        return NULL;
    }
    FILE *file = fopen(path, "r");
    free(path);
    if (file == NULL) return NULL;
    char *line = NULL;
    size_t capacity = 0;
    ssize_t length = getline(&line, &capacity, file);
    int saved_errno = errno;
    bool empty_file = length < 0 && feof(file);
    fclose(file);
    if (length < 0) {
        free(line);
        if (empty_file) return strdup("");
        errno = saved_errno == 0 ? EIO : saved_errno;
        return NULL;
    }
    while (length > 0 && (line[length - 1] == '\n' ||
                          line[length - 1] == '\r' ||
                          line[length - 1] == ' ' ||
                          line[length - 1] == '\t')) {
        line[--length] = '\0';
    }
    return line;
}

/* Match a controller name in the whitespace-separated cgroup lists. */
static bool control_list_has(const char *list, const char *name) {
    size_t name_length = strlen(name);
    const char *cursor = list;
    while (*cursor != '\0') {
        while (*cursor == ' ' || *cursor == '\t' || *cursor == '\n') ++cursor;
        const char *start = cursor;
        while (*cursor != '\0' && *cursor != ' ' && *cursor != '\t' &&
               *cursor != '\n') ++cursor;
        if ((size_t)(cursor - start) == name_length &&
            memcmp(start, name, name_length) == 0) return true;
    }
    return false;
}

/* Ensure cpuset is available to children, enabling it only in a delegated scope. */
static bool enable_cpuset_controller(const char *cgroup_path,
                                       bool may_enable,
                                       char *error, size_t error_size) {
    char *available = read_control_value(cgroup_path,
                                                  "cgroup.controllers");
    if (available == NULL) {
        snprintf(error, error_size, "cannot read cgroup controllers in '%s': %s",
                 cgroup_path, strerror(errno));
        return false;
    }
    if (!control_list_has(available, "cpuset")) {
        snprintf(error, error_size,
                 "cpuset controller is unavailable in '%s'", cgroup_path);
        free(available);
        return false;
    }
    free(available);

    char *enabled = read_control_value(cgroup_path,
                                                "cgroup.subtree_control");
    if (enabled == NULL) {
        snprintf(error, error_size, "cannot read cgroup subtree control in '%s': %s",
                 cgroup_path, strerror(errno));
        return false;
    }
    bool is_enabled = control_list_has(enabled, "cpuset");
    free(enabled);
    if (is_enabled) return true;
    if (!may_enable) {
        snprintf(error, error_size,
                 "cpuset is not enabled for child cgroups in '%s'; use a delegated cgroup v2 subtree with cpuset enabled",
                 cgroup_path);
        return false;
    }

    char *subtree_path = cgroupv2_child_path(cgroup_path, "cgroup.subtree_control");
    if (subtree_path == NULL) {
        snprintf(error, error_size, "out of memory enabling cpuset controller");
        return false;
    }
    bool enabled_cpuset = cgroupv2_write_control(subtree_path, "+cpuset");
    if (!enabled_cpuset) {
        snprintf(error, error_size,
                 "cannot enable cpuset in '%s': %s", cgroup_path,
                 strerror(errno));
    }
    free(subtree_path);
    return enabled_cpuset;
}

/* Select an available CPU and capture the memory nodes inherited by the scope. */
static bool select_isolation_cpu_sets(const char *parent_path,
                                        char **isolated_cpu,
                                        char **worker_cpus,
                                        char **memory_nodes,
                                        char *error, size_t error_size) {
    char *effective_cpus = read_control_value(
        parent_path, "cpuset.cpus.effective");
    char *effective_mems = read_control_value(
        parent_path, "cpuset.mems.effective");
    char *available_cpus = read_control_value(
        parent_path, "cpuset.cpus.exclusive.effective");
    int exclusive_errno = errno;
    char *partition = read_control_value(
        parent_path, "cpuset.cpus.partition");
    int partition_errno = errno;
    if (effective_cpus == NULL || effective_cpus[0] == '\0' ||
        effective_mems == NULL || effective_mems[0] == '\0') {
        snprintf(error, error_size,
                 "cannot read non-empty effective CPU and memory sets from '%s'",
                 parent_path);
        goto failed;
    }

    const char *candidate_cpus = available_cpus;
    if (partition == NULL && partition_errno == ENOENT &&
        available_cpus == NULL && exclusive_errno == ENOENT) {
        /* The cgroup v2 root has no partition or exclusive-effective files. */
        candidate_cpus = effective_cpus;
    } else if (partition == NULL) {
        snprintf(error, error_size,
                 "cannot inspect cpuset partition in '%s': %s",
                 parent_path, strerror(partition_errno));
        goto failed;
    } else if (strcmp(partition, "root") != 0 &&
               strcmp(partition, "isolated") != 0) {
        snprintf(error, error_size,
                 "parent cgroup '%s' is not a valid cpuset partition root (%s)",
                 parent_path, partition);
        goto failed;
    } else if (available_cpus == NULL) {
        snprintf(error, error_size,
                 "cannot read exclusive CPUs from '%s': %s",
                 parent_path, strerror(exclusive_errno));
        goto failed;
    }

    if (candidate_cpus == NULL || candidate_cpus[0] == '\0') {
        snprintf(error, error_size,
                 "no exclusive CPU is available in '%s'", parent_path);
        goto failed;
    }
    if (!process_isolation_choose_cpu_sets(candidate_cpus, effective_cpus,
                                          isolated_cpu, worker_cpus,
                                          error, error_size)) goto failed;
    *memory_nodes = effective_mems;
    effective_mems = NULL;
    free(effective_cpus);
    free(available_cpus);
    free(partition);
    return true;

failed:
    free(effective_cpus);
    free(effective_mems);
    free(available_cpus);
    free(partition);
    return false;
}

/* Initialize a cgroup's cpuset before attaching any benchmark processes. */
static bool configure_cpuset(const char *cgroup_path, const char *cpus,
                               const char *memory_nodes,
                               const char *exclusive,
                               char *error, size_t error_size) {
    const char *controls[] = {"cpuset.cpus", "cpuset.mems"};
    const char *values[] = {cpus, memory_nodes};
    for (size_t i = 0; i < sizeof(controls) / sizeof(controls[0]); ++i) {
        char *path = cgroupv2_child_path(cgroup_path, controls[i]);
        if (path == NULL) {
            snprintf(error, error_size, "out of memory configuring cpuset");
            return false;
        }
        bool written = cgroupv2_write_control(path, values[i]);
        if (!written) {
            int saved_errno = errno;
            snprintf(error, error_size, "cannot set '%s': %s", path,
                     strerror(saved_errno));
            free(path);
            errno = saved_errno;
            return false;
        }
        free(path);
    }
    if (exclusive != NULL) {
        char *path = cgroupv2_child_path(cgroup_path, "cpuset.cpus.exclusive");
        if (path == NULL) {
            snprintf(error, error_size,
                     "out of memory configuring exclusive CPUs");
            return false;
        }
        bool written = cgroupv2_write_control(path, exclusive);
        if (!written) {
            int saved_errno = errno;
            snprintf(error, error_size, "cannot set '%s': %s", path,
                     strerror(saved_errno));
            free(path);
            errno = saved_errno;
            return false;
        }
        free(path);
    }
    return true;
}

/* Set a partition state and reject kernel-reported invalid partitions. */
static bool set_partition_state(const char *cgroup_path, const char *state,
                                  char *error, size_t error_size) {
    char *path = cgroupv2_child_path(cgroup_path, "cpuset.cpus.partition");
    if (path == NULL) {
        snprintf(error, error_size, "out of memory setting cpuset partition");
        return false;
    }
    if (!cgroupv2_write_control(path, state)) {
        int saved_errno = errno;
        snprintf(error, error_size, "cannot set '%s' to '%s': %s", path,
                 state, strerror(saved_errno));
        free(path);
        errno = saved_errno;
        return false;
    }
    free(path);
    char *actual = read_control_value(cgroup_path,
                                               "cpuset.cpus.partition");
    if (actual == NULL) {
        snprintf(error, error_size, "cannot verify cpuset partition in '%s': %s",
                 cgroup_path, strerror(errno));
        return false;
    }
    bool valid = strcmp(actual, state) == 0;
    if (!valid) {
        snprintf(error, error_size,
                 "kernel rejected cpuset partition in '%s' (%s)",
                 cgroup_path, actual);
    }
    free(actual);
    return valid;
}

/* Select the exclusive CPU and inherited sets for the cgroup strategy. */
static bool cgroupv2_isolation_select(ProcessIsolation *isolation,
                                      const char *parent_path,
                                      bool may_enable,
                                      char *error, size_t error_size) {
    if (!enable_cpuset_controller(parent_path, may_enable, error,
                                  error_size)) return false;
    char *isolated_cpu = NULL;
    if (!select_isolation_cpu_sets(parent_path, &isolated_cpu,
                                   &isolation->worker_cpus,
                                   &isolation->memory_nodes,
                                   error, error_size)) return false;
    char *end = NULL;
    errno = 0;
    unsigned long cpu = strtoul(isolated_cpu, &end, 10);
    bool valid = errno == 0 && end != isolated_cpu && *end == '\0' &&
                 cpu <= UINT_MAX;
    if (!valid) {
        snprintf(error, error_size,
                 "cannot parse selected isolated CPU '%s'", isolated_cpu);
        free(isolated_cpu);
        return false;
    }
    isolation->cpu = (unsigned)cpu;
    snprintf(isolation->isolated_cpu, sizeof(isolation->isolated_cpu), "%s",
             isolated_cpu);
    free(isolated_cpu);
    return true;
}

/* Create the intermediate root partition selected by this strategy. */
static bool cgroupv2_isolation_configure_base(ProcessIsolation *isolation,
                                              const char *base_path,
                                              char *error,
                                              size_t error_size) {
    if (!configure_cpuset(base_path, isolation->isolated_cpu,
                          isolation->memory_nodes, isolation->isolated_cpu,
                          error, error_size) ||
        !set_partition_state(base_path, "root", error, error_size)) {
        return false;
    }
    return enable_cpuset_controller(base_path, true, error, error_size);
}

/* Isolate terminal descendants and leave the benchmark worker on other CPUs. */
static bool cgroupv2_isolation_configure_process_groups(
    ProcessIsolation *isolation, const char *terminal_path,
    const char *worker_path, char *error, size_t error_size) {
    if (!configure_cpuset(terminal_path, isolation->isolated_cpu,
                          isolation->memory_nodes, isolation->isolated_cpu,
                          error, error_size) ||
        !set_partition_state(terminal_path, "isolated", error, error_size) ||
        !configure_cpuset(worker_path, isolation->worker_cpus,
                          isolation->memory_nodes, NULL,
                          error, error_size)) return false;
    char *worker_effective = read_control_value(worker_path,
                                                "cpuset.cpus.effective");
    if (worker_effective == NULL || worker_effective[0] == '\0') {
        snprintf(error, error_size,
                 "benchmark worker has no effective CPUs outside isolated CPU %s",
                 isolation->isolated_cpu);
        free(worker_effective);
        return false;
    }
    free(worker_effective);
    return true;
}

/* Cgroup placement itself attaches the terminal for this strategy. */
static bool cgroupv2_isolation_attach_terminal(ProcessIsolation *isolation,
                                               pid_t terminal_pid,
                                               char *error,
                                               size_t error_size) {
    (void)isolation;
    (void)terminal_pid;
    (void)error;
    (void)error_size;
    return true;
}

/* Check that the terminal's child cgroup became an isolated partition. */
static MonitorIsolation cgroupv2_isolation_check(
    ProcessIsolation *isolation, const char *terminal_path,
    pid_t terminal_pid, pid_t worker_pid, const char **detail) {
    (void)terminal_pid;
    (void)worker_pid;
    char *partition = read_control_value(terminal_path,
                                         "cpuset.cpus.partition");
    char *effective_cpus = read_control_value(terminal_path,
                                              "cpuset.cpus.effective");
    if (partition == NULL || effective_cpus == NULL) {
        int saved_errno = errno;
        snprintf(isolation->detail, sizeof(isolation->detail),
                 "cannot inspect terminal cpuset isolation: %s",
                 strerror(saved_errno));
        free(partition);
        free(effective_cpus);
        if (detail != NULL) *detail = isolation->detail;
        return MONITOR_ISOLATION_UNAVAILABLE;
    }
    if (strcmp(partition, "isolated") != 0 ||
        strcmp(effective_cpus, isolation->isolated_cpu) != 0) {
        snprintf(isolation->detail, sizeof(isolation->detail),
                 "terminal cpuset partition is not isolated on CPU %s (%s, CPUs %s)",
                 isolation->isolated_cpu, partition, effective_cpus);
        free(partition);
        free(effective_cpus);
        if (detail != NULL) *detail = isolation->detail;
        return MONITOR_ISOLATION_OUTSIDE;
    }
    free(partition);
    free(effective_cpus);
    snprintf(isolation->detail, sizeof(isolation->detail),
             "terminal isolated on CPU %s; worker is in its separate cgroup",
             isolation->isolated_cpu);
    if (detail != NULL) *detail = isolation->detail;
    return MONITOR_ISOLATION_INSIDE;
}

/* Release the dynamically selected sets after the scope has been removed. */
static void cgroupv2_isolation_destroy(ProcessIsolation *isolation) {
    free(isolation->worker_cpus);
    free(isolation->memory_nodes);
    isolation->worker_cpus = NULL;
    isolation->memory_nodes = NULL;
}

const ProcessIsolationOps *cgroupv2_process_isolation_ops(void) {
    static const ProcessIsolationOps operations = {
        .select = cgroupv2_isolation_select,
        .configure_base = cgroupv2_isolation_configure_base,
        .configure_process_groups =
            cgroupv2_isolation_configure_process_groups,
        .attach_terminal = cgroupv2_isolation_attach_terminal,
        .check = cgroupv2_isolation_check,
        .destroy = cgroupv2_isolation_destroy,
    };
    return &operations;
}
