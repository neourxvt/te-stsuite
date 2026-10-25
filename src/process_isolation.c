#define _GNU_SOURCE
#include "process_isolation.h"

#include <errno.h>
#include <limits.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
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

/* The taskset strategy uses affinity and leaves cgroup configuration alone. */
static bool taskset_configure_base(ProcessIsolation *isolation,
                                   const char *base_path,
                                   char *error, size_t error_size) {
    (void)isolation;
    (void)base_path;
    (void)error;
    (void)error_size;
    return true;
}

/* The taskset strategy does not alter the cgroup CPU sets. */
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

/* Verify the terminal and worker retained the original CPU 0 affinity. */
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

static const ProcessIsolationOps taskset_ops = {
    .select = taskset_select,
    .configure_base = taskset_configure_base,
    .configure_process_groups = taskset_configure_process_groups,
    .attach_terminal = taskset_attach_terminal,
    .check = taskset_check,
    .destroy = taskset_destroy,
};

/* Select the shared strategy implementation for a validated mode. */
bool process_isolation_init(ProcessIsolation *isolation,
                            ProcessIsolationMode mode) {
    if (isolation == NULL) return false;
    memset(isolation, 0, sizeof(*isolation));
    isolation->mode = mode;
    if (mode == PROCESS_ISOLATION_CGROUPV2) {
        isolation->ops = cgroupv2_process_isolation_ops();
    } else if (mode == PROCESS_ISOLATION_TASKSET) {
        isolation->ops = &taskset_ops;
    }
    return isolation->ops != NULL;
}

/* Select CPUs and prepare each strategy before cgroups are created. */
bool process_isolation_select(ProcessIsolation *isolation,
                              const char *parent_path, bool may_enable,
                              char *error, size_t error_size) {
    return isolation != NULL && isolation->ops != NULL &&
           isolation->ops->select(isolation, parent_path, may_enable,
                                  error, error_size);
}

/* Configure the intermediate cgroup through the selected strategy. */
bool process_isolation_configure_base(ProcessIsolation *isolation,
                                      const char *base_path,
                                      char *error, size_t error_size) {
    return isolation != NULL && isolation->ops != NULL &&
           isolation->ops->configure_base(isolation, base_path,
                                          error, error_size);
}

/* Configure terminal and worker CPU sets through the selected strategy. */
bool process_isolation_configure_process_groups(
    ProcessIsolation *isolation, const char *terminal_path,
    const char *worker_path, char *error, size_t error_size) {
    return isolation != NULL && isolation->ops != NULL &&
           isolation->ops->configure_process_groups(
               isolation, terminal_path, worker_path, error, error_size);
}

/* Apply any per-process setup after moving the terminal into its cgroup. */
bool process_isolation_attach_terminal(ProcessIsolation *isolation,
                                      pid_t terminal_pid,
                                      char *error, size_t error_size) {
    return isolation != NULL && isolation->ops != NULL &&
           isolation->ops->attach_terminal(isolation, terminal_pid,
                                           error, error_size);
}

/* Verify the selected CPU isolation strategy after the worker reports ready. */
MonitorIsolation process_isolation_check(ProcessIsolation *isolation,
                                         const char *terminal_path,
                                         pid_t terminal_pid, pid_t worker_pid,
                                         const char **detail) {
    if (isolation == NULL || isolation->ops == NULL) {
        if (detail != NULL) *detail = "process isolation is unavailable";
        return MONITOR_ISOLATION_UNAVAILABLE;
    }
    return isolation->ops->check(isolation, terminal_path, terminal_pid,
                                 worker_pid, detail);
}

/* Release strategy-specific CPU lists before the scope itself is destroyed. */
void process_isolation_destroy(ProcessIsolation *isolation) {
    if (isolation == NULL || isolation->ops == NULL) return;
    isolation->ops->destroy(isolation);
    isolation->ops = NULL;
}

/* Accept the two CPU placement methods exposed through the TOML setting. */
bool process_isolation_parse(const char *name, ProcessIsolationMode *mode) {
    if (name == NULL || mode == NULL) return false;
    if (strcmp(name, "cgroupv2") == 0) {
        *mode = PROCESS_ISOLATION_CGROUPV2;
        return true;
    }
    if (strcmp(name, "taskset") == 0) {
        *mode = PROCESS_ISOLATION_TASKSET;
        return true;
    }
    return false;
}

/* Return the canonical TOML spelling for the selected CPU placement method. */
const char *process_isolation_name(ProcessIsolationMode mode) {
    switch (mode) {
        case PROCESS_ISOLATION_CGROUPV2: return "cgroupv2";
        case PROCESS_ISOLATION_TASKSET: return "taskset";
    }
    return "unknown";
}

/* Pin one process to a single CPU before it launches terminal descendants. */
bool process_isolation_pin_pid(pid_t pid, unsigned cpu) {
    if (pid <= 0 || cpu >= CPU_SETSIZE) {
        errno = EINVAL;
        return false;
    }
    cpu_set_t cpus;
    CPU_ZERO(&cpus);
    CPU_SET(cpu, &cpus);
    return sched_setaffinity(pid, sizeof(cpus), &cpus) == 0;
}

/* Verify that a process still has affinity to only the selected CPU. */
bool process_isolation_pid_is_pinned(pid_t pid, unsigned cpu) {
    if (pid <= 0 || cpu >= CPU_SETSIZE) return false;
    cpu_set_t cpus;
    CPU_ZERO(&cpus);
    if (sched_getaffinity(pid, sizeof(cpus), &cpus) != 0 ||
        !CPU_ISSET(cpu, &cpus)) return false;
    for (unsigned candidate = 0; candidate < CPU_SETSIZE; ++candidate) {
        if (candidate != cpu && CPU_ISSET(candidate, &cpus)) return false;
    }
    return true;
}

typedef struct {
    unsigned first;
    unsigned last;
} CpuRange;

typedef struct {
    CpuRange *items;
    size_t count;
    size_t capacity;
} CpuRangeList;

/* Parse the kernel's comma-separated CPU ranges without expanding them. */
static bool parse_cpu_ranges(const char *text, CpuRangeList *ranges) {
    const char *cursor = text;
    while (*cursor == ' ' || *cursor == '\t' || *cursor == '\n') ++cursor;
    if (*cursor == '\0') return false;

    while (*cursor != '\0') {
        char *end = NULL;
        errno = 0;
        unsigned long first = strtoul(cursor, &end, 10);
        if (errno != 0 || end == cursor || first > UINT_MAX) return false;
        cursor = end;
        unsigned long last = first;
        if (*cursor == '-') {
            ++cursor;
            errno = 0;
            last = strtoul(cursor, &end, 10);
            if (errno != 0 || end == cursor || last > UINT_MAX || last < first) {
                return false;
            }
            cursor = end;
        }
        if (*cursor != ',' && *cursor != '\0' && *cursor != '\n') return false;
        if (ranges->count == ranges->capacity) {
            size_t capacity = ranges->capacity == 0 ? 4 : ranges->capacity * 2;
            CpuRange *items = realloc(ranges->items, capacity * sizeof(*items));
            if (items == NULL) return false;
            ranges->items = items;
            ranges->capacity = capacity;
        }
        ranges->items[ranges->count++] = (CpuRange){
            .first = (unsigned)first,
            .last = (unsigned)last,
        };
        if (*cursor == '\n') {
            while (*cursor == '\n' || *cursor == ' ' || *cursor == '\t') ++cursor;
            return *cursor == '\0';
        }
        if (*cursor == ',') {
            ++cursor;
            if (*cursor == '\0') return false;
        }
    }
    return ranges->count > 0;
}

/* Check whether an effective CPU list includes the selected exclusive CPU. */
static bool cpu_range_list_contains(const CpuRangeList *ranges, unsigned cpu) {
    for (size_t i = 0; i < ranges->count; ++i) {
        if (cpu >= ranges->items[i].first && cpu <= ranges->items[i].last) {
            return true;
        }
    }
    return false;
}

/* Append one range to a dynamically sized cpulist. */
static bool append_cpu_range(char **text, size_t *length, size_t *capacity,
                             unsigned first, unsigned last) {
    char range[48];
    int written = first == last
                      ? snprintf(range, sizeof(range), "%u", first)
                      : snprintf(range, sizeof(range), "%u-%u", first, last);
    if (written < 0 || (size_t)written >= sizeof(range)) return false;
    size_t separator = *length == 0 ? 0 : 1;
    size_t required = *length + separator + (size_t)written + 1;
    if (required > *capacity) {
        size_t next = *capacity == 0 ? 64 : *capacity;
        while (next < required) next *= 2;
        char *grown = realloc(*text, next);
        if (grown == NULL) return false;
        *text = grown;
        *capacity = next;
    }
    if (separator != 0) (*text)[(*length)++] = ',';
    memcpy(*text + *length, range, (size_t)written);
    *length += (size_t)written;
    (*text)[*length] = '\0';
    return true;
}

/* Remove the selected CPU from the worker's inherited CPU set. */
static char *cpu_list_without(const CpuRangeList *ranges, unsigned selected) {
    char *text = NULL;
    size_t length = 0;
    size_t capacity = 0;
    for (size_t i = 0; i < ranges->count; ++i) {
        CpuRange range = ranges->items[i];
        if (selected < range.first || selected > range.last) {
            if (!append_cpu_range(&text, &length, &capacity,
                                  range.first, range.last)) goto failed;
            continue;
        }
        if (range.first < selected &&
            !append_cpu_range(&text, &length, &capacity,
                              range.first, selected - 1)) goto failed;
        if (selected < range.last &&
            !append_cpu_range(&text, &length, &capacity,
                              selected + 1, range.last)) goto failed;
    }
    return text;

failed:
    free(text);
    return NULL;
}

/* Select one exclusive CPU while keeping the benchmark worker off that CPU. */
bool process_isolation_choose_cpu_sets(const char *available_cpus,
                                       const char *effective_cpus,
                                       char **isolated_cpu,
                                       char **worker_cpus,
                                       char *error, size_t error_size) {
    if (available_cpus == NULL || effective_cpus == NULL ||
        isolated_cpu == NULL || worker_cpus == NULL) return false;
    *isolated_cpu = NULL;
    *worker_cpus = NULL;

    CpuRangeList available = {0};
    CpuRangeList effective = {0};
    if (!parse_cpu_ranges(available_cpus, &available) ||
        !parse_cpu_ranges(effective_cpus, &effective)) {
        snprintf(error, error_size, "cannot parse the delegated CPU lists");
        free(available.items);
        free(effective.items);
        return false;
    }

    unsigned selected = UINT_MAX;
    for (size_t i = 0; i < available.count; ++i) {
        for (size_t j = 0; j < effective.count; ++j) {
            unsigned first = available.items[i].first > effective.items[j].first
                                 ? available.items[i].first
                                 : effective.items[j].first;
            unsigned last = available.items[i].last < effective.items[j].last
                                ? available.items[i].last
                                : effective.items[j].last;
            if (first <= last && first < selected) selected = first;
        }
    }
    if (selected == UINT_MAX || !cpu_range_list_contains(&effective, selected)) {
        snprintf(error, error_size,
                 "no online CPU is available for an exclusive partition");
        free(available.items);
        free(effective.items);
        return false;
    }

    char cpu_text[32];
    snprintf(cpu_text, sizeof(cpu_text), "%u", selected);
    char *remaining = cpu_list_without(&effective, selected);
    if (remaining == NULL) {
        snprintf(error, error_size, "out of memory selecting worker CPUs");
        free(available.items);
        free(effective.items);
        return false;
    }
    if (remaining[0] == '\0') {
        snprintf(error, error_size,
                 "CPU isolation needs another available CPU for the benchmark worker");
        free(remaining);
        free(available.items);
        free(effective.items);
        return false;
    }
    char *isolated = strdup(cpu_text);
    if (isolated == NULL) {
        snprintf(error, error_size, "out of memory selecting an isolated CPU");
        free(remaining);
        free(available.items);
        free(effective.items);
        return false;
    }

    *isolated_cpu = isolated;
    *worker_cpus = remaining;
    free(available.items);
    free(effective.items);
    return true;
}
