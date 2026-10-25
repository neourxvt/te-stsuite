#define _GNU_SOURCE
#include "cvtebench.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* Locate build-copied workload data beside this executable, not its build path. */
bool benchmark_data_path(char *path, size_t path_size, const char *filename) {
    char executable[PATH_MAX];
    ssize_t length = readlink("/proc/self/exe", executable,
                              sizeof(executable) - 1);
    if (length < 0 || (size_t)length >= sizeof(executable) - 1) return false;
    executable[length] = '\0';

    char *separator = strrchr(executable, '/');
    if (separator == NULL) return false;
    *separator = '\0';

    int written = snprintf(path, path_size, "%s/data/%s", executable, filename);
    return written >= 0 && (size_t)written < path_size;
}
