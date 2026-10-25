#include "te-stsuite.h"

/* Scroll only the top part of the screen while preserving the final row. */
int bench_scrolling_top_region_prepare(BenchData *data,
                                       const BenchContext *context,
                                       char *error, size_t error_size) {
    (void)error;
    (void)error_size;
    if (!buffer_appendf(&data->setup, "\033[?1049h\033[2;%ur",
                        context->rows)) {
        return -1;
    }
    return buffer_append(&data->payload, "y\n", 2) ? 0 : -1;
}
