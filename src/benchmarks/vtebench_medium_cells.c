#include "te-stsuite.h"

#include <stdio.h>

/* Replay the escape-heavy Vim reference session captured by vtebench. */
int bench_medium_cells_prepare(BenchData *data, const BenchContext *context,
                               char *error, size_t error_size) {
    (void)context;
    if (!buffer_append(&data->payload, te_stsuite_medium_cells,
                       te_stsuite_medium_cells_length)) {
        snprintf(error, error_size,
                 "cannot load embedded medium-cells benchmark data");
        return -1;
    }
    return 0;
}
