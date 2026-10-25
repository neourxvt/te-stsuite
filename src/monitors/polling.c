#define _GNU_SOURCE
#include "cvtebench.h"
#include "backend.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

typedef struct {
    pid_t pid;
    pid_t parent;
    uint64_t ticks;
    uint64_t rss_bytes;
} ProcEntry;

/* State for one independently measured startup or benchmark interval. */
typedef struct {
    pid_t roots[8];
    size_t root_count;
    pid_t worker_pid;
    long ticks_per_second;
    uint64_t start_ns;
    uint64_t last_sample_ns;
    uint64_t start_ticks;
    uint64_t current_ticks;
    uint64_t last_rss_bytes;
    uint64_t current_rss_bytes;
    uint64_t peak_rss_bytes;
    long double rss_integral;
    bool observed;
    bool active;
} PollingState;

/* Return a monotonic timestamp used for workload timing and polling. */
uint64_t monotonic_now_ns(void) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return 0;
    return (uint64_t)now.tv_sec * 1000000000ULL + (uint64_t)now.tv_nsec;
}

/* Read the kernel stat fields needed for a process or thread. */
static bool read_stat(pid_t pid, pid_t *parent, uint64_t *ticks,
                      uint64_t *rss_pages) {
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

    pid_t parsed_parent = 0;
    uint64_t user_ticks = 0;
    uint64_t system_ticks = 0;
    uint64_t resident_pages = 0;
    unsigned field = 3;
    char *save = NULL;
    for (char *token = strtok_r(cursor, " \t\n", &save); token != NULL;
         token = strtok_r(NULL, " \t\n", &save), ++field) {
        char *end = NULL;
        if (field == 4) {
            long parsed = strtol(token, &end, 10);
            if (end == token || *end != '\0') break;
            parsed_parent = (pid_t)parsed;
        } else if (field == 14) {
            user_ticks = strtoull(token, &end, 10);
            if (end == token || *end != '\0') break;
        } else if (field == 15) {
            system_ticks = strtoull(token, &end, 10);
            if (end == token || *end != '\0') break;
        } else if (field == 24) {
            long long parsed = strtoll(token, &end, 10);
            if (end == token || *end != '\0') break;
            if (parsed > 0) resident_pages = (uint64_t)parsed;
            break;
        }
    }
    free(line);
    if (field < 24) return false;
    if (parent != NULL) *parent = parsed_parent;
    if (ticks != NULL) *ticks = user_ticks + system_ticks;
    if (rss_pages != NULL) *rss_pages = resident_pages;
    return true;
}

/* Read RSS in bytes from the process status file. */
static uint64_t read_rss_bytes(pid_t pid, uint64_t fallback_pages) {
    char path[128];
    snprintf(path, sizeof(path), "/proc/%ld/status", (long)pid);
    FILE *file = fopen(path, "r");
    if (file == NULL) return fallback_pages * (uint64_t)sysconf(_SC_PAGESIZE);

    char *line = NULL;
    size_t capacity = 0;
    uint64_t result = fallback_pages * (uint64_t)sysconf(_SC_PAGESIZE);
    while (getline(&line, &capacity, file) >= 0) {
        unsigned long long kibibytes;
        if (sscanf(line, "VmRSS: %llu kB", &kibibytes) == 1) {
            result = (uint64_t)kibibytes * 1024ULL;
            break;
        }
    }
    free(line);
    fclose(file);
    return result;
}

/* Sum CPU time for every task while counting a process's RSS only once. */
static bool read_process(pid_t pid, ProcEntry *entry) {
    uint64_t ignored_ticks = 0;
    uint64_t pages = 0;
    if (!read_stat(pid, &entry->parent, &ignored_ticks, &pages)) return false;
    entry->pid = pid;
    entry->ticks = 0;
    entry->rss_bytes = read_rss_bytes(pid, pages);

    char path[128];
    snprintf(path, sizeof(path), "/proc/%ld/task", (long)pid);
    DIR *tasks = opendir(path);
    if (tasks == NULL) {
        entry->ticks = ignored_ticks;
        return true;
    }
    struct dirent *item;
    while ((item = readdir(tasks)) != NULL) {
        char *end = NULL;
        long tid = strtol(item->d_name, &end, 10);
        if (end == item->d_name || *end != '\0' || tid <= 0) continue;
        uint64_t task_ticks = 0;
        if (read_stat((pid_t)tid, NULL, &task_ticks, NULL)) {
            entry->ticks += task_ticks;
        }
    }
    closedir(tasks);
    return true;
}

/* Sort proc entries so parent lookups can use binary search. */
static int compare_proc_entry(const void *left, const void *right) {
    const ProcEntry *a = left;
    const ProcEntry *b = right;
    return (a->pid > b->pid) - (a->pid < b->pid);
}

/* Locate a proc record by PID in the sorted process snapshot. */
static ProcEntry *find_proc_entry(ProcEntry *entries, size_t count, pid_t pid) {
    ProcEntry key = {.pid = pid};
    return bsearch(&key, entries, count, sizeof(*entries), compare_proc_entry);
}

/* Test whether an entry belongs to the terminal tree and is not the worker. */
static bool belongs_to_terminal(PollingState *monitor, ProcEntry *entries,
                                size_t count, pid_t pid) {
    ProcEntry *entry = find_proc_entry(entries, count, pid);
    for (unsigned depth = 0; entry != NULL && depth < 128; ++depth) {
        if (entry->pid == monitor->worker_pid) return false;
        for (size_t i = 0; i < monitor->root_count; ++i) {
            if (entry->pid == monitor->roots[i]) return true;
        }
        entry = find_proc_entry(entries, count, entry->parent);
    }
    return false;
}

/* Snapshot terminal-tree CPU ticks and resident memory from procfs. */
static bool take_snapshot(PollingState *monitor, uint64_t *ticks,
                          uint64_t *rss_bytes) {
    *ticks = 0;
    *rss_bytes = 0;
    DIR *proc = opendir("/proc");
    if (proc == NULL) return false;

    ProcEntry *entries = NULL;
    size_t count = 0;
    size_t capacity = 0;
    struct dirent *item;
    while ((item = readdir(proc)) != NULL) {
        char *end = NULL;
        long pid = strtol(item->d_name, &end, 10);
        if (end == item->d_name || *end != '\0' || pid <= 0) continue;
        if (count == capacity) {
            size_t next_capacity = capacity == 0 ? 128 : capacity * 2;
            ProcEntry *next = realloc(entries, next_capacity * sizeof(*entries));
            if (next == NULL) break;
            entries = next;
            capacity = next_capacity;
        }
        ProcEntry entry;
        if (read_process((pid_t)pid, &entry)) entries[count++] = entry;
    }
    closedir(proc);
    qsort(entries, count, sizeof(*entries), compare_proc_entry);

    bool observed = false;
    for (size_t i = 0; i < count; ++i) {
        if (!belongs_to_terminal(monitor, entries, count, entries[i].pid)) continue;
        observed = true;
        *ticks += entries[i].ticks;
        *rss_bytes += entries[i].rss_bytes;
    }
    free(entries);
    return observed;
}

/* Create process-tree accounting for the terminal launcher. */
static void *polling_create(pid_t root_pid, CgroupScope *scope) {
    (void)scope;
    PollingState *monitor = calloc(1, sizeof(*monitor));
    if (monitor == NULL) return NULL;
    monitor->roots[0] = root_pid;
    monitor->root_count = 1;
    monitor->ticks_per_second = sysconf(_SC_CLK_TCK);
    if (monitor->ticks_per_second <= 0) monitor->ticks_per_second = 100;
    monitor->observed = take_snapshot(monitor, &monitor->current_ticks,
                                      &monitor->current_rss_bytes);
    return monitor;
}

/* Free process-tree accounting state. */
static void polling_destroy(void *state) {
    PollingState *monitor = state;
    free(monitor);
}

/* Exclude the benchmark worker and follow a terminal process spawned by it. */
static void polling_set_worker(void *state, pid_t worker_pid,
                               pid_t terminal_pid) {
    PollingState *monitor = state;
    monitor->worker_pid = worker_pid;
    if (terminal_pid <= 0 || monitor->root_count >= 8) return;
    for (size_t i = 0; i < monitor->root_count; ++i) {
        if (monitor->roots[i] == terminal_pid) return;
    }
    monitor->roots[monitor->root_count++] = terminal_pid;
}

/* Begin a new measurement interval using a fresh process-tree snapshot. */
static void polling_begin(void *state, uint64_t start_ns) {
    PollingState *monitor = state;
    monitor->observed |= take_snapshot(monitor, &monitor->current_ticks,
                                       &monitor->current_rss_bytes);
    monitor->start_ns = start_ns;
    monitor->last_sample_ns = start_ns;
    monitor->start_ticks = monitor->current_ticks;
    monitor->peak_rss_bytes = monitor->current_rss_bytes;
    monitor->rss_integral = 0;
    monitor->active = true;
}

/* Accumulate time-weighted RSS while refreshing CPU and peak memory. */
static void polling_tick(void *state, uint64_t now_ns) {
    PollingState *monitor = state;
    if (monitor == NULL || !monitor->active) return;
    uint64_t ticks = 0;
    uint64_t rss = 0;
    monitor->observed |= take_snapshot(monitor, &ticks, &rss);
    if (now_ns > monitor->last_sample_ns) {
        monitor->rss_integral += (long double)monitor->current_rss_bytes *
                                 (long double)(now_ns - monitor->last_sample_ns);
    }
    monitor->last_sample_ns = now_ns;
    monitor->current_ticks = ticks;
    monitor->current_rss_bytes = rss;
    if (rss > monitor->peak_rss_bytes) monitor->peak_rss_bytes = rss;
}

/* Finish a measurement and convert accumulated counters to comparable units. */
static MonitorMetrics polling_end(void *state, uint64_t end_ns) {
    PollingState *monitor = state;
    polling_tick(monitor, end_ns);
    MonitorMetrics metrics = {.resources_available = monitor->observed};
    uint64_t elapsed_ns = end_ns > monitor->start_ns
                              ? end_ns - monitor->start_ns
                              : 0;
    metrics.wall_seconds = (double)elapsed_ns / 1000000000.0;
    uint64_t cpu_delta = monitor->current_ticks >= monitor->start_ticks
                             ? monitor->current_ticks - monitor->start_ticks
                             : 0;
    metrics.cpu_seconds = (double)cpu_delta /
                          (double)monitor->ticks_per_second;
    if (metrics.wall_seconds > 0) {
        metrics.average_memory_bytes =
            (double)(monitor->rss_integral / (long double)elapsed_ns);
    }
    metrics.peak_memory_bytes = monitor->peak_rss_bytes;
    monitor->active = false;
    return metrics;
}

/* Expose the polling implementation through the shared monitor interface. */
const MonitorBackendOps *polling_monitor_ops(void) {
    static const MonitorBackendOps operations = {
        .create = polling_create,
        .destroy = polling_destroy,
        .set_worker = polling_set_worker,
        .begin = polling_begin,
        .tick = polling_tick,
        .end = polling_end,
    };
    return &operations;
}
