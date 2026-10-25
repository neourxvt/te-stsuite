#include "te-stsuite.h"

#include <stdio.h>

/* Replay the synchronized-output Vim reference session. */
int bench_sync_medium_cells_prepare(BenchData *data,
                                    const BenchContext *context,
                                    char *error, size_t error_size) {
    (void)context;
    char path[4096];
    if (!benchmark_data_path(path, sizeof(path), "sync_medium_cells.vim") ||
        !buffer_load_file(&data->payload, path)) {
        snprintf(error, error_size,
                 "cannot read sync_medium_cells.vim from data/");
        return -1;
    }
    return 0;
}
