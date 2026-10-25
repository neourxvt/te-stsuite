#define _GNU_SOURCE
#include "process_isolation.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
