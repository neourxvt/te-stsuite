#include "cvtebench.h"

#include <stdio.h>

/* Replay the synchronized-output Vim reference session. */
int bench_sync_medium_cells_prepare(BenchData *data,
                                    const BenchContext *context,
                                    char *error, size_t error_size) {
    (void)context;
    char path[4096];
    snprintf(path, sizeof(path), "%s/sync_medium_cells.vim",
             CVTEBENCH_DATA_DIR);
    if (!buffer_load_file(&data->payload, path)) {
        snprintf(error, error_size,
                 "cannot read sync_medium_cells.vim from data/");
        return -1;
    }
    return 0;
}
