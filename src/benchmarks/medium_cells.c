#include "cvtebench.h"

#include <stdio.h>

/* Replay the escape-heavy Vim reference session captured by vtebench. */
int bench_medium_cells_prepare(BenchData *data, const BenchContext *context,
                               char *error, size_t error_size) {
    (void)context;
    char path[4096];
    snprintf(path, sizeof(path), "%s/medium_cells.vim", CVTEBENCH_DATA_DIR);
    if (!buffer_load_file(&data->payload, path)) {
        snprintf(error, error_size, "cannot read medium_cells.vim from data/");
        return -1;
    }
    return 0;
}
