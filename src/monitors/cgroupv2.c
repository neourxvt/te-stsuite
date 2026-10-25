#define _GNU_SOURCE
#include "backend.h"
#include "../process_isolation.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <time.h>
#include <unistd.h>

/* Keep cgroup paths and setup failures alongside each terminal launch. */
struct CgroupScope {
    char *base_path;
    char *terminal_path;
    char *worker_path;
    char *terminal_membership;
    char *worker_membership;
    ProcessIsolation isolation;
    char error[512];
    bool ready;
    bool base_created;
    bool terminal_created;
    bool worker_created;
};

/* State for one interval of cgroup CPU and memory accounting. */
typedef struct {
    CgroupScope *scope;
    uint64_t start_ns;
    uint64_t last_sample_ns;
    uint64_t start_cpu_usec;
    uint64_t current_memory_bytes;
    uint64_t sampled_peak_bytes;
    long double memory_integral;
    bool start_cpu_valid;
    bool start_memory_valid;
    bool peak_reset;
    bool active;
} CgroupMonitorState;

/* Decode the octal escapes used for mount paths in /proc/self/mountinfo. */
static void decode_mount_path(char *path) {
    char *input = path;
    char *output = path;
    while (*input != '\0') {
        if (input[0] == '\\' && input[1] >= '0' && input[1] <= '7' &&
            input[2] >= '0' && input[2] <= '7' &&
            input[3] >= '0' && input[3] <= '7') {
            *output++ = (char)((input[1] - '0') * 64 +
                               (input[2] - '0') * 8 + (input[3] - '0'));
            input += 4;
        } else {
            *output++ = *input++;
        }
    }
    *output = '\0';
}

/* Locate the unified cgroup mount and map this process's membership into it. */
static char *current_cgroup_directory(char *error, size_t error_size) {
    char membership[4096] = {0};
    FILE *file = fopen("/proc/self/cgroup", "r");
    if (file == NULL) {
        snprintf(error, error_size, "cannot read /proc/self/cgroup: %s",
                 strerror(errno));
        return NULL;
    }
    char *line = NULL;
    size_t capacity = 0;
    bool found = false;
    while (getline(&line, &capacity, file) >= 0) {
        if (strncmp(line, "0::", 3) == 0) {
            size_t length = strcspn(line + 3, "\n");
            if (length < sizeof(membership)) {
                memcpy(membership, line + 3, length);
                membership[length] = '\0';
                found = true;
            }
            break;
        }
    }
    free(line);
    fclose(file);
    if (!found) {
        snprintf(error, error_size,
                 "the process has no unified cgroup v2 membership");
        return NULL;
    }

    file = fopen("/proc/self/mountinfo", "r");
    if (file == NULL) {
        snprintf(error, error_size, "cannot read /proc/self/mountinfo: %s",
                 strerror(errno));
        return NULL;
    }
    line = NULL;
    capacity = 0;
    char *resolved = NULL;
    while (getline(&line, &capacity, file) >= 0) {
        char *tokens[128];
        size_t count = 0;
        char *save = NULL;
        for (char *token = strtok_r(line, " \n", &save);
             token != NULL && count < sizeof(tokens) / sizeof(tokens[0]);
             token = strtok_r(NULL, " \n", &save)) {
            tokens[count++] = token;
        }
        size_t separator_index = 0;
        while (separator_index < count &&
               strcmp(tokens[separator_index], "-") != 0) {
            ++separator_index;
        }
        if (separator_index + 1 >= count || separator_index < 6 ||
            strcmp(tokens[separator_index + 1], "cgroup2") != 0) continue;

        char mount_root[4096];
        char mount_point[4096];
        if (strlen(tokens[3]) >= sizeof(mount_root) ||
            strlen(tokens[4]) >= sizeof(mount_point)) continue;
        strcpy(mount_root, tokens[3]);
        strcpy(mount_point, tokens[4]);
        decode_mount_path(mount_root);
        decode_mount_path(mount_point);
        size_t root_length = strlen(mount_root);
        if (strncmp(membership, mount_root, root_length) != 0 ||
            (root_length > 1 && membership[root_length] != '\0' &&
             membership[root_length] != '/')) continue;

        const char *suffix = membership + root_length;
        if (root_length == 1 && mount_root[0] == '/') suffix = membership;
        size_t mount_length = strlen(mount_point);
        bool mount_has_slash = mount_length > 0 &&
                               mount_point[mount_length - 1] == '/';
        if (mount_has_slash && suffix[0] == '/') ++suffix;
        const char *separator = !mount_has_slash && suffix[0] != '/'
                                    ? "/"
                                    : "";
        if (asprintf(&resolved, "%s%s%s", mount_point, separator, suffix) < 0) {
            resolved = NULL;
            break;
        }
        if (resolved[0] == '\0') {
            free(resolved);
            resolved = strdup("/");
        }
        break;
    }
    free(line);
    fclose(file);
    if (resolved == NULL) {
        snprintf(error, error_size,
                 "cannot map the unified cgroup membership to a cgroup2 mount");
    }
    return resolved;
}

/* Read the unified membership path used by /proc/<pid>/cgroup. */
static char *current_membership_path(void) {
    FILE *file = fopen("/proc/self/cgroup", "r");
    if (file == NULL) return NULL;
    char *line = NULL;
    size_t capacity = 0;
    char *membership = NULL;
    while (getline(&line, &capacity, file) >= 0) {
        if (strncmp(line, "0::", 3) != 0) continue;
        size_t length = strcspn(line + 3, "\n");
        membership = strndup(line + 3, length);
        break;
    }
    free(line);
    fclose(file);
    return membership;
}

/* Write one short value to a cgroup control file. */
static bool write_control(const char *path, const char *value) {
    int fd = open(path, O_WRONLY | O_CLOEXEC);
    if (fd < 0) return false;
    size_t length = strlen(value);
    size_t offset = 0;
    while (offset < length) {
        ssize_t written = write(fd, value + offset, length - offset);
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0) {
            int saved_errno = written < 0 ? errno : EIO;
            close(fd);
            errno = saved_errno;
            return false;
        }
        offset += (size_t)written;
    }
    return close(fd) == 0;
}

/* Build a child path without relying on fixed system cgroup path lengths. */
static char *child_path(const char *parent, const char *name) {
    char *path = NULL;
    size_t parent_length = strlen(parent);
    if (parent_length == 1 && parent[0] == '/') {
        if (asprintf(&path, "/%s", name) < 0) return NULL;
    } else if (parent_length > 0 && parent[parent_length - 1] == '/') {
        if (asprintf(&path, "%s%s", parent, name) < 0) return NULL;
    } else if (asprintf(&path, "%s/%s", parent, name) < 0) {
        return NULL;
    }
    return path;
}

/* Read one cgroup control file and return its trimmed contents. */
static char *read_control_value(const char *cgroup_path, const char *name) {
    char *path = child_path(cgroup_path, name);
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
    char *available = read_control_value(cgroup_path, "cgroup.controllers");
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

    char *enabled = read_control_value(cgroup_path, "cgroup.subtree_control");
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

    char *subtree_path = child_path(cgroup_path, "cgroup.subtree_control");
    if (subtree_path == NULL) {
        snprintf(error, error_size, "out of memory enabling cpuset controller");
        return false;
    }
    bool enabled_cpuset = write_control(subtree_path, "+cpuset");
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
    char *effective_cpus = read_control_value(parent_path,
                                              "cpuset.cpus.effective");
    char *effective_mems = read_control_value(parent_path,
                                              "cpuset.mems.effective");
    char *available_cpus = read_control_value(
        parent_path, "cpuset.cpus.exclusive.effective");
    int exclusive_errno = errno;
    char *partition = read_control_value(parent_path,
                                         "cpuset.cpus.partition");
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
                             const char *memory_nodes, const char *exclusive,
                             char *error, size_t error_size) {
    const char *controls[] = {"cpuset.cpus", "cpuset.mems"};
    const char *values[] = {cpus, memory_nodes};
    for (size_t i = 0; i < sizeof(controls) / sizeof(controls[0]); ++i) {
        char *path = child_path(cgroup_path, controls[i]);
        if (path == NULL) {
            snprintf(error, error_size, "out of memory configuring cpuset");
            return false;
        }
        bool written = write_control(path, values[i]);
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
        char *path = child_path(cgroup_path, "cpuset.cpus.exclusive");
        if (path == NULL) {
            snprintf(error, error_size,
                     "out of memory configuring exclusive CPUs");
            return false;
        }
        bool written = write_control(path, exclusive);
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
    char *path = child_path(cgroup_path, "cpuset.cpus.partition");
    if (path == NULL) {
        snprintf(error, error_size, "out of memory setting cpuset partition");
        return false;
    }
    if (!write_control(path, state)) {
        int saved_errno = errno;
        snprintf(error, error_size, "cannot set '%s' to '%s': %s", path,
                 state, strerror(saved_errno));
        free(path);
        errno = saved_errno;
        return false;
    }
    free(path);
    char *actual = read_control_value(cgroup_path, "cpuset.cpus.partition");
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
        !set_partition_state(terminal_path, "isolated", error,
                             error_size) ||
        !configure_cpuset(worker_path, isolation->worker_cpus,
                          isolation->memory_nodes, NULL,
                          error, error_size)) return false;
    char *worker_effective = read_control_value(
        worker_path, "cpuset.cpus.effective");
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

/* Register cgroup v2 with the shared process isolation operations table. */
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

/* Check whether a cgroup path ends at the requested subgroup. */
static bool has_final_component(const char *path, const char *component) {
    size_t path_length = strlen(path);
    size_t component_length = strlen(component);
    return path_length > component_length &&
           path[path_length - component_length - 1] == '/' &&
           memcmp(path + path_length - component_length, component,
                  component_length) == 0;
}

/* Trim the runner subgroup to recover the delegated scope's root path. */
static bool trim_final_component(char *path, const char *component) {
    if (!has_final_component(path, component)) return false;
    size_t path_length = strlen(path);
    size_t component_length = strlen(component);
    size_t parent_length = path_length - component_length - 1;
    path[parent_length == 0 ? 1 : parent_length] = '\0';
    return true;
}

/* Move the runner below the delegated root so it can enable child controllers. */
static bool move_to_controller_subgroup(const char *parent,
                                       const char *subgroup,
                                       char *error, size_t error_size) {
    char *subgroup_path = child_path(parent, subgroup);
    if (subgroup_path == NULL) {
        snprintf(error, error_size, "out of memory creating runner subgroup");
        return false;
    }
    if (mkdir(subgroup_path, 0700) != 0) {
        snprintf(error, error_size, "cannot create runner subgroup '%s': %s",
                 subgroup_path, strerror(errno));
        free(subgroup_path);
        return false;
    }
    char *processes_path = child_path(subgroup_path, "cgroup.procs");
    if (processes_path == NULL) {
        snprintf(error, error_size, "out of memory moving runner to subgroup");
        rmdir(subgroup_path);
        free(subgroup_path);
        return false;
    }
    char pid_text[32];
    snprintf(pid_text, sizeof(pid_text), "%ld", (long)getpid());
    bool moved = write_control(processes_path, pid_text);
    if (!moved) {
        int saved_errno = errno;
        snprintf(error, error_size, "cannot move runner into delegated subgroup: %s",
                 strerror(saved_errno));
        rmdir(subgroup_path);
        errno = saved_errno;
    }
    free(subgroup_path);
    free(processes_path);
    return moved;
}

/* Enable CPU and memory accounting before adding child cgroups. */
static bool enable_accounting_controllers(const char *cgroup_path,
                                          char *error, size_t error_size) {
    char *available_path = child_path(cgroup_path, "cgroup.controllers");
    char *enabled_path = child_path(cgroup_path, "cgroup.subtree_control");
    if (available_path == NULL || enabled_path == NULL) {
        snprintf(error, error_size, "out of memory enabling cgroup controllers");
        free(available_path);
        free(enabled_path);
        return false;
    }
    FILE *file = fopen(available_path, "r");
    if (file == NULL) {
        snprintf(error, error_size, "cannot read '%s': %s", available_path,
                 strerror(errno));
        free(available_path);
        free(enabled_path);
        return false;
    }
    char *line = NULL;
    size_t capacity = 0;
    bool cpu_available = false;
    bool memory_available = false;
    if (getline(&line, &capacity, file) >= 0) {
        char *save = NULL;
        for (char *token = strtok_r(line, " \t\n", &save);
             token != NULL; token = strtok_r(NULL, " \t\n", &save)) {
            if (strcmp(token, "cpu") == 0) cpu_available = true;
            if (strcmp(token, "memory") == 0) memory_available = true;
        }
    }
    free(line);
    fclose(file);
    if (!cpu_available || !memory_available) {
        snprintf(error, error_size,
                 "CPU and memory controllers are not both available in '%s'",
                 cgroup_path);
        free(available_path);
        free(enabled_path);
        return false;
    }
    bool enabled = write_control(enabled_path, "+cpu +memory");
    if (!enabled) {
        snprintf(error, error_size, "cannot enable CPU and memory controllers in '%s': %s",
                 cgroup_path, strerror(errno));
    }
    free(available_path);
    free(enabled_path);
    return enabled;
}

/* Remove partially-created leaves after a failed delegated scope setup. */
static void remove_partial_scope(CgroupScope *scope) {
    if (scope->worker_created && scope->worker_path != NULL &&
        rmdir(scope->worker_path) == 0) {
        scope->worker_created = false;
    }
    if (scope->terminal_created && scope->terminal_path != NULL &&
        rmdir(scope->terminal_path) == 0) {
        scope->terminal_created = false;
    }
    if (scope->base_created && scope->base_path != NULL &&
        rmdir(scope->base_path) == 0) {
        scope->base_created = false;
    }
}

/* Explain cgroup creation failures using the parent permissions and mount state. */
static void describe_cgroup_create_error(CgroupScope *scope,
                                         const char *parent_path,
                                         const char *child,
                                         int saved_errno) {
    struct stat parent_stat;
    struct statvfs filesystem_stat;
    char parent_detail[192] = {0};
    char mount_detail[64] = {0};
    if (stat(parent_path, &parent_stat) == 0) {
        snprintf(parent_detail, sizeof(parent_detail),
                 "; parent mode %04o owner %lu:%lu, process uid %lu",
                 (unsigned)(parent_stat.st_mode & 07777),
                 (unsigned long)parent_stat.st_uid,
                 (unsigned long)parent_stat.st_gid,
                 (unsigned long)geteuid());
    }
    if (statvfs(parent_path, &filesystem_stat) == 0 &&
        (filesystem_stat.f_flag & ST_RDONLY) != 0) {
        snprintf(mount_detail, sizeof(mount_detail),
                 "; cgroup mount is read-only");
    }
    snprintf(scope->error, sizeof(scope->error),
             "cannot create cgroup '%s': %s%s%s", child,
             strerror(saved_errno), parent_detail, mount_detail);
    errno = saved_errno;
}

/* Create terminal and worker groups using the selected isolation strategy. */
CgroupScope *cgroup_scope_create(ProcessIsolationMode process_isolation) {
    CgroupScope *scope = calloc(1, sizeof(*scope));
    if (scope == NULL) return NULL;
    if (!process_isolation_init(&scope->isolation, process_isolation)) {
        snprintf(scope->error, sizeof(scope->error),
                 "unknown process isolation mode");
        return scope;
    }
    char error[sizeof(scope->error)] = {0};
    char *parent = current_cgroup_directory(error, sizeof(error));
    char *membership = current_membership_path();
    if (parent == NULL) {
        snprintf(scope->error, sizeof(scope->error), "%s", error);
        free(membership);
        return scope;
    }
    if (membership == NULL) {
        snprintf(scope->error, sizeof(scope->error),
                 "cannot read unified cgroup membership");
        free(parent);
        return scope;
    }

    /* A delegated systemd scope keeps its runner in this named child group. */
    const char *delegated_subgroup = getenv("TE_STSUITE_DELEGATED_SUBGROUP");
    bool delegated_systemd_scope = delegated_subgroup != NULL;
    if (delegated_systemd_scope &&
        strcmp(delegated_subgroup, "te-stsuite-controller") != 0) {
        snprintf(scope->error, sizeof(scope->error),
                 "unexpected delegated systemd subgroup '%s'",
                 delegated_subgroup);
        free(parent);
        free(membership);
        return scope;
    }
    if (delegated_systemd_scope) {
        bool parent_in_subgroup = has_final_component(parent, delegated_subgroup);
        bool membership_in_subgroup =
            has_final_component(membership, delegated_subgroup);
        if (parent_in_subgroup != membership_in_subgroup) {
            snprintf(scope->error, sizeof(scope->error),
                     "runner cgroup paths disagree about subgroup '%s'",
                     delegated_subgroup);
            free(parent);
            free(membership);
            return scope;
        }
        if (parent_in_subgroup) {
            trim_final_component(parent, delegated_subgroup);
            trim_final_component(membership, delegated_subgroup);
        } else if (!move_to_controller_subgroup(parent, delegated_subgroup,
                                                scope->error,
                                                sizeof(scope->error))) {
            free(parent);
            free(membership);
            return scope;
        }
    }
    if (delegated_systemd_scope &&
        !enable_accounting_controllers(parent, scope->error,
                                       sizeof(scope->error))) goto failed;
    if (!process_isolation_select(&scope->isolation, parent,
                                  delegated_systemd_scope, scope->error,
                                  sizeof(scope->error))) goto failed;

    char group_name[96];
    char worker_name[112];
    struct timespec now = {0};
    clock_gettime(CLOCK_MONOTONIC, &now);
    snprintf(group_name, sizeof(group_name), "te-stsuite-%ld-%llu",
             (long)getpid(),
             (unsigned long long)now.tv_nsec);
    snprintf(worker_name, sizeof(worker_name), "%s-worker", group_name);
    scope->base_path = child_path(parent, group_name);
    char *membership_base = child_path(membership, group_name);
    if (scope->base_path == NULL || membership_base == NULL) {
        snprintf(scope->error, sizeof(scope->error), "out of memory creating cgroup path");
        free(membership_base);
        goto failed;
    }
    scope->terminal_path = child_path(scope->base_path, "terminal");
    scope->worker_path = child_path(parent, worker_name);
    scope->terminal_membership = child_path(membership_base, "terminal");
    scope->worker_membership = child_path(membership, worker_name);
    free(membership_base);
    if (scope->terminal_path == NULL || scope->worker_path == NULL ||
        scope->terminal_membership == NULL || scope->worker_membership == NULL) {
        snprintf(scope->error, sizeof(scope->error),
                 "out of memory creating cgroup child paths");
        goto failed;
    }
    if (mkdir(scope->base_path, 0700) != 0) {
        int saved_errno = errno;
        describe_cgroup_create_error(scope, parent, scope->base_path,
                                     saved_errno);
        goto failed;
    }
    scope->base_created = true;

    char isolation_error[sizeof(scope->error)] = {0};
    if (!process_isolation_configure_base(
            &scope->isolation, scope->base_path, isolation_error,
            sizeof(isolation_error))) {
        snprintf(scope->error, sizeof(scope->error), "%s", isolation_error);
        goto failed;
    }
    char accounting_error[sizeof(scope->error)] = {0};
    if (!enable_accounting_controllers(scope->base_path, accounting_error,
                                       sizeof(accounting_error)) &&
        delegated_systemd_scope) {
        snprintf(scope->error, sizeof(scope->error), "%s", accounting_error);
        goto failed;
    }
    if (mkdir(scope->terminal_path, 0700) == 0) {
        scope->terminal_created = true;
    } else {
        snprintf(scope->error, sizeof(scope->error),
                 "cannot create terminal cgroup: %s", strerror(errno));
        goto failed;
    }
    if (mkdir(scope->worker_path, 0700) == 0) {
        scope->worker_created = true;
    } else {
        snprintf(scope->error, sizeof(scope->error),
                 "cannot create worker cgroup: %s", strerror(errno));
        goto failed;
    }
    if (!process_isolation_configure_process_groups(
            &scope->isolation, scope->terminal_path, scope->worker_path,
            isolation_error, sizeof(isolation_error))) {
        snprintf(scope->error, sizeof(scope->error), "%s", isolation_error);
        goto failed;
    }
    scope->ready = true;
    free(parent);
    free(membership);
    return scope;

failed:
    free(parent);
    free(membership);
    remove_partial_scope(scope);
    return scope;
}

/* Kill only processes in this private scope and remove its empty children. */
void cgroup_scope_destroy(CgroupScope *scope) {
    if (scope == NULL) return;
    if (scope->terminal_created && scope->terminal_path != NULL) {
        char *kill_path = child_path(scope->terminal_path, "cgroup.kill");
        if (kill_path != NULL) {
            write_control(kill_path, "1");
            free(kill_path);
        }
    }
    if (scope->worker_created && scope->worker_path != NULL) {
        char *kill_path = child_path(scope->worker_path, "cgroup.kill");
        if (kill_path != NULL) {
            write_control(kill_path, "1");
            free(kill_path);
        }
    }
    for (unsigned attempt = 0; attempt < 10; ++attempt) {
        remove_partial_scope(scope);
        bool base_exists = scope->base_path != NULL &&
                           access(scope->base_path, F_OK) == 0;
        bool worker_exists = scope->worker_path != NULL &&
                             access(scope->worker_path, F_OK) == 0;
        if (!base_exists && !worker_exists) break;
        struct timespec pause = {.tv_sec = 0, .tv_nsec = 10000000};
        nanosleep(&pause, NULL);
    }
    free(scope->base_path);
    free(scope->terminal_path);
    free(scope->worker_path);
    free(scope->terminal_membership);
    free(scope->worker_membership);
    process_isolation_destroy(&scope->isolation);
    free(scope);
}

/* Move the forked launcher before it can create any terminal descendants. */
bool cgroup_scope_attach_terminal(CgroupScope *scope, pid_t terminal_pid) {
    if (scope == NULL || !scope->ready) return false;
    char *path = child_path(scope->terminal_path, "cgroup.procs");
    if (path == NULL) return false;
    char pid_text[32];
    snprintf(pid_text, sizeof(pid_text), "%ld", (long)terminal_pid);
    bool moved = write_control(path, pid_text);
    int saved_errno = errno;
    free(path);
    if (moved) {
        char isolation_error[sizeof(scope->error)] = {0};
        moved = process_isolation_attach_terminal(
            &scope->isolation, terminal_pid, isolation_error,
            sizeof(isolation_error));
        if (!moved) {
            saved_errno = errno;
            snprintf(scope->error, sizeof(scope->error), "%s",
                     isolation_error[0] == '\0'
                         ? "cannot apply process isolation"
                         : isolation_error);
        }
    }
    if (!moved) {
        scope->ready = false;
        if (scope->error[0] == '\0') {
            snprintf(scope->error, sizeof(scope->error),
                     "cannot place terminal in cgroup: %s",
                     strerror(saved_errno));
        }
    }
    return moved;
}

/* Return the worker sibling directory or a sentinel when setup was unavailable. */
const char *cgroup_scope_worker_path(const CgroupScope *scope) {
    return scope != NULL && scope->ready && scope->worker_path != NULL
               ? scope->worker_path
               : "-";
}

/* Return the selected CPU after the configured process mode is ready. */
const char *cgroup_scope_selected_cpu(const CgroupScope *scope) {
    return scope != NULL && scope->ready &&
                   scope->isolation.isolated_cpu[0] != '\0'
               ? scope->isolation.isolated_cpu
               : "-";
}

/* Join the in-terminal worker to its sibling without moving the terminal. */
bool cgroupv2_join_worker(const char *path) {
    if (path == NULL || strcmp(path, "-") == 0) return false;
    char *control = child_path(path, "cgroup.procs");
    if (control == NULL) return false;
    char pid_text[32];
    snprintf(pid_text, sizeof(pid_text), "%ld", (long)getpid());
    bool moved = write_control(control, pid_text);
    free(control);
    return moved;
}

/* Read a process's unified membership to verify cgroup placement. */
static char *process_cgroup(pid_t pid) {
    char path[128];
    snprintf(path, sizeof(path), "/proc/%ld/cgroup", (long)pid);
    FILE *file = fopen(path, "r");
    if (file == NULL) return NULL;
    char *line = NULL;
    size_t capacity = 0;
    char *membership = NULL;
    while (getline(&line, &capacity, file) >= 0) {
        if (strncmp(line, "0::", 3) != 0) continue;
        size_t length = strcspn(line + 3, "\n");
        membership = strndup(line + 3, length);
        break;
    }
    free(line);
    fclose(file);
    return membership;
}

/* Accept a cgroup or any nested child under that cgroup. */
static bool membership_within(const char *membership, const char *expected) {
    if (membership == NULL || expected == NULL) return false;
    size_t length = strlen(expected);
    return strncmp(membership, expected, length) == 0 &&
           (membership[length] == '\0' || membership[length] == '/');
}

/* Read a process parent PID while tolerating spaces in its command name. */
static bool read_process_parent(pid_t pid, pid_t *parent) {
    char path[128];
    snprintf(path, sizeof(path), "/proc/%ld/stat", (long)pid);
    FILE *file = fopen(path, "r");
    if (file == NULL) return false;
    char *line = NULL;
    size_t capacity = 0;
    ssize_t length = getline(&line, &capacity, file);
    fclose(file);
    if (length <= 0) {
        free(line);
        return false;
    }
    char *cursor = strrchr(line, ')');
    if (cursor == NULL) {
        free(line);
        return false;
    }
    ++cursor;
    char state[8];
    long parsed_parent = 0;
    bool ok = sscanf(cursor, " %7s %ld", state, &parsed_parent) == 2 &&
              parsed_parent >= 0;
    free(line);
    if (ok) *parent = (pid_t)parsed_parent;
    return ok;
}

/* Collect parent links once so the cgroup check can inspect terminal descendants. */
static bool collect_process_parents(pid_t **pids_out, pid_t **parents_out,
                                    size_t *count_out) {
    DIR *proc = opendir("/proc");
    if (proc == NULL) return false;
    pid_t *pids = NULL;
    pid_t *parents = NULL;
    size_t count = 0;
    size_t capacity = 0;
    struct dirent *item;
    while ((item = readdir(proc)) != NULL) {
        char *end = NULL;
        long parsed = strtol(item->d_name, &end, 10);
        if (end == item->d_name || *end != '\0' || parsed <= 0) continue;
        pid_t parent = 0;
        if (!read_process_parent((pid_t)parsed, &parent)) continue;
        if (count == capacity) {
            size_t next = capacity == 0 ? 128 : capacity * 2;
            pid_t *new_pids = realloc(pids, next * sizeof(*pids));
            if (new_pids == NULL) {
                free(pids);
                free(parents);
                closedir(proc);
                return false;
            }
            pids = new_pids;
            pid_t *new_parents = realloc(parents, next * sizeof(*parents));
            if (new_parents == NULL) {
                free(pids);
                free(parents);
                closedir(proc);
                return false;
            }
            parents = new_parents;
            capacity = next;
        }
        pids[count] = (pid_t)parsed;
        parents[count] = parent;
        ++count;
    }
    closedir(proc);
    *pids_out = pids;
    *parents_out = parents;
    *count_out = count;
    return true;
}

/* Exclude the worker sibling and find every live descendant of the terminal. */
static bool is_terminal_descendant(pid_t pid, pid_t root_pid, pid_t worker_pid,
                                   const pid_t *pids, const pid_t *parents,
                                   size_t count) {
    pid_t current = pid;
    for (size_t depth = 0; depth < 128 && current > 0; ++depth) {
        if (current == worker_pid) return false;
        if (current == root_pid) return true;
        size_t index = 0;
        while (index < count && pids[index] != current) ++index;
        if (index == count) return false;
        current = parents[index];
    }
    return false;
}

/* Ensure live descendants inherited the terminal scope rather than escaping it. */
static bool descendants_stay_in_scope(const CgroupScope *scope,
                                      pid_t terminal_pid, pid_t worker_pid,
                                      bool *inspectable) {
    pid_t *pids = NULL;
    pid_t *parents = NULL;
    size_t count = 0;
    *inspectable = collect_process_parents(&pids, &parents, &count);
    if (!*inspectable) return false;
    bool inside = true;
    for (size_t i = 0; i < count; ++i) {
        if (!is_terminal_descendant(pids[i], terminal_pid, worker_pid,
                                   pids, parents, count)) continue;
        char *membership = process_cgroup(pids[i]);
        if (membership == NULL) {
            char process_path[128];
            snprintf(process_path, sizeof(process_path), "/proc/%ld",
                     (long)pids[i]);
            if (access(process_path, F_OK) == 0) *inspectable = false;
            free(membership);
            if (!*inspectable) {
                inside = false;
                break;
            }
            continue;
        }
        bool process_inside = membership_within(membership,
                                                scope->terminal_membership);
        free(membership);
        if (!process_inside) {
            inside = false;
            break;
        }
    }
    free(pids);
    free(parents);
    return inside;
}

/* Verify terminal and worker placement and the isolated partition state. */
MonitorIsolation cgroup_scope_check(CgroupScope *scope, pid_t terminal_pid,
                                    pid_t worker_pid, bool worker_moved,
                                    const char **detail) {
    if (detail != NULL) *detail = NULL;
    if (scope == NULL || !scope->ready) {
        if (detail != NULL) *detail = cgroup_scope_error(scope);
        return MONITOR_ISOLATION_UNAVAILABLE;
    }
    if (!worker_moved) {
        if (detail != NULL) *detail = "worker could not join its separate cgroup";
        return MONITOR_ISOLATION_OUTSIDE;
    }
    char *terminal_membership = process_cgroup(terminal_pid);
    char *worker_membership = process_cgroup(worker_pid);
    char terminal_proc_path[128];
    char worker_proc_path[128];
    snprintf(terminal_proc_path, sizeof(terminal_proc_path), "/proc/%ld",
             (long)terminal_pid);
    snprintf(worker_proc_path, sizeof(worker_proc_path), "/proc/%ld",
             (long)worker_pid);
    if ((terminal_membership == NULL &&
         access(terminal_proc_path, F_OK) == 0) ||
        (worker_membership == NULL && access(worker_proc_path, F_OK) == 0)) {
        free(terminal_membership);
        free(worker_membership);
        if (detail != NULL) *detail = "cannot inspect process cgroup membership";
        return MONITOR_ISOLATION_UNAVAILABLE;
    }
    bool terminal_inside = membership_within(terminal_membership,
                                            scope->terminal_membership);
    bool worker_inside = worker_membership == NULL
                             ? worker_moved
                             : membership_within(worker_membership,
                                                 scope->worker_membership);
    free(terminal_membership);
    free(worker_membership);
    if (!terminal_inside) {
        if (detail != NULL) *detail = "terminal process is outside its cgroup";
        return MONITOR_ISOLATION_OUTSIDE;
    }
    if (!worker_inside) {
        if (detail != NULL) *detail = "worker is outside its separate cgroup";
        return MONITOR_ISOLATION_OUTSIDE;
    }
    bool inspectable = true;
    bool descendants_inside = descendants_stay_in_scope(
        scope, terminal_pid, worker_pid, &inspectable);
    if (!inspectable) {
        if (detail != NULL) *detail = "cannot inspect terminal descendants in procfs";
        return MONITOR_ISOLATION_UNAVAILABLE;
    }
    if (!descendants_inside) {
        if (detail != NULL) *detail =
            "a terminal descendant is outside the terminal cgroup";
        return MONITOR_ISOLATION_OUTSIDE;
    }
    return process_isolation_check(&scope->isolation, scope->terminal_path,
                                   terminal_pid, worker_pid, detail);
}

/* Expose setup failure so text and JSON reports explain missing cgroup support. */
const char *cgroup_scope_error(const CgroupScope *scope) {
    if (scope == NULL) return "out of memory creating cgroup scope";
    return scope->error[0] == '\0' ? "cgroup scope is unavailable" : scope->error;
}

/* Read a decimal counter from a cgroup v2 control file. */
static bool read_counter(const char *path, uint64_t *value) {
    FILE *file = fopen(path, "r");
    if (file == NULL) return false;
    unsigned long long parsed = 0;
    bool ok = fscanf(file, "%llu", &parsed) == 1;
    fclose(file);
    if (ok) *value = (uint64_t)parsed;
    return ok;
}

/* Read cumulative CPU use from cpu.stat's usage_usec entry. */
static bool read_cpu_usec(const CgroupScope *scope, uint64_t *value) {
    char *path = child_path(scope->terminal_path, "cpu.stat");
    if (path == NULL) return false;
    FILE *file = fopen(path, "r");
    free(path);
    if (file == NULL) return false;
    char key[64];
    unsigned long long parsed = 0;
    bool found = false;
    while (fscanf(file, "%63s %llu", key, &parsed) == 2) {
        if (strcmp(key, "usage_usec") == 0) {
            *value = (uint64_t)parsed;
            found = true;
            break;
        }
    }
    fclose(file);
    return found;
}

/* Sample memory.current for a time-weighted average and sampled fallback peak. */
static bool read_memory_current(const CgroupScope *scope, uint64_t *value) {
    char *path = child_path(scope->terminal_path, "memory.current");
    if (path == NULL) return false;
    bool ok = read_counter(path, value);
    free(path);
    return ok;
}

/* Read memory.peak when the kernel exposes the cgroup v2 peak counter. */
static bool read_memory_peak(const CgroupScope *scope, uint64_t *value) {
    char *path = child_path(scope->terminal_path, "memory.peak");
    if (path == NULL) return false;
    bool ok = read_counter(path, value);
    free(path);
    return ok;
}

/* Reset memory.peak where supported so every interval starts independently. */
static bool reset_memory_peak(const CgroupScope *scope) {
    char *path = child_path(scope->terminal_path, "memory.peak");
    if (path == NULL) return false;
    bool ok = write_control(path, "0");
    free(path);
    return ok;
}

/* Allocate a cgroup backend bound to the private terminal scope. */
static void *cgroup_monitor_create(pid_t terminal_pid, CgroupScope *scope) {
    (void)terminal_pid;
    CgroupMonitorState *state = calloc(1, sizeof(*state));
    if (state != NULL) state->scope = scope;
    return state;
}

/* Release state after the cgroup scope has stopped collecting. */
static void cgroup_monitor_destroy(void *state) {
    free(state);
}

/* The worker lives in a sibling group and is excluded by construction. */
static void cgroup_monitor_set_worker(void *state, pid_t worker_pid,
                                      pid_t terminal_pid) {
    (void)state;
    (void)worker_pid;
    (void)terminal_pid;
}

/* Establish counter baselines and reset the peak for a new measurement. */
static void cgroup_monitor_begin(void *opaque, uint64_t start_ns) {
    CgroupMonitorState *state = opaque;
    state->start_ns = start_ns;
    state->last_sample_ns = start_ns;
    state->memory_integral = 0;
    state->start_cpu_valid = state->scope->ready &&
                             read_cpu_usec(state->scope,
                                           &state->start_cpu_usec);
    state->start_memory_valid = state->scope->ready &&
                                read_memory_current(state->scope,
                                                    &state->current_memory_bytes);
    state->sampled_peak_bytes = state->start_memory_valid
                                    ? state->current_memory_bytes
                                    : 0;
    state->peak_reset = state->scope->ready && reset_memory_peak(state->scope);
    state->active = true;
}

/* Integrate memory.current and retain the highest observed sample. */
static void cgroup_monitor_tick(void *opaque, uint64_t now_ns) {
    CgroupMonitorState *state = opaque;
    if (state == NULL || !state->active || !state->scope->ready) return;
    uint64_t memory = 0;
    if (read_memory_current(state->scope, &memory)) {
        if (now_ns > state->last_sample_ns) {
            state->memory_integral +=
                (long double)state->current_memory_bytes *
                (long double)(now_ns - state->last_sample_ns);
        }
        state->current_memory_bytes = memory;
        if (memory > state->sampled_peak_bytes) state->sampled_peak_bytes = memory;
    } else {
        state->start_memory_valid = false;
    }
    state->last_sample_ns = now_ns;
}

/* Convert cgroup counters to seconds and time-weighted memory bytes. */
static MonitorMetrics cgroup_monitor_end(void *opaque, uint64_t end_ns) {
    CgroupMonitorState *state = opaque;
    cgroup_monitor_tick(state, end_ns);
    MonitorMetrics metrics = {0};
    uint64_t elapsed_ns = end_ns > state->start_ns
                              ? end_ns - state->start_ns
                              : 0;
    metrics.wall_seconds = (double)elapsed_ns / 1000000000.0;
    uint64_t end_cpu_usec = 0;
    bool end_cpu_valid = state->scope->ready &&
                         read_cpu_usec(state->scope, &end_cpu_usec);
    uint64_t peak = 0;
    bool kernel_peak_valid = state->peak_reset &&
                             read_memory_peak(state->scope, &peak);
    metrics.resources_available = state->start_cpu_valid && end_cpu_valid &&
                                  state->start_memory_valid;
    if (end_cpu_valid && end_cpu_usec >= state->start_cpu_usec) {
        metrics.cpu_seconds =
            (double)(end_cpu_usec - state->start_cpu_usec) / 1000000.0;
    }
    if (elapsed_ns > 0 && state->start_memory_valid) {
        metrics.average_memory_bytes =
            (double)(state->memory_integral / (long double)elapsed_ns);
    }
    metrics.peak_memory_bytes = kernel_peak_valid ? peak
                                                  : state->sampled_peak_bytes;
    state->active = false;
    return metrics;
}

/* Expose the cgroup v2 implementation through the shared monitor interface. */
const MonitorBackendOps *cgroupv2_monitor_ops(void) {
    static const MonitorBackendOps operations = {
        .create = cgroup_monitor_create,
        .destroy = cgroup_monitor_destroy,
        .set_worker = cgroup_monitor_set_worker,
        .begin = cgroup_monitor_begin,
        .tick = cgroup_monitor_tick,
        .end = cgroup_monitor_end,
    };
    return &operations;
}
