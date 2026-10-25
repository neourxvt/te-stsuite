#include "te-stsuite.h"

/* Restrict scrolling to the first rows minus the final terminal row. */
int bench_scrolling_bottom_region_prepare(BenchData *data,
                                          const BenchContext *context,
                                          char *error, size_t error_size) {
    (void)error;
    (void)error_size;
    unsigned bottom = context->rows > 1 ? context->rows - 1 : 1;
    if (!buffer_appendf(&data->setup, "\033[?1049h\033[1;%ur", bottom)) {
        return -1;
    }
    return buffer_append(&data->payload, "y\n", 2) ? 0 : -1;
}
