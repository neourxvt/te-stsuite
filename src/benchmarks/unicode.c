#include "te-stsuite.h"

#include <stdio.h>

/* Replay the bundled Unicode symbol stream used by vtebench. */
int bench_unicode_prepare(BenchData *data, const BenchContext *context,
                          char *error, size_t error_size) {
    (void)context;
    char path[4096];
    if (!benchmark_data_path(path, sizeof(path), "unicode_symbols.txt") ||
        !buffer_append(&data->setup, "\033[?1049h", 8) ||
        !buffer_load_file(&data->payload, path)) {
        snprintf(error, error_size, "cannot read unicode_symbols.txt from data/");
        return -1;
    }
    return 0;
}
