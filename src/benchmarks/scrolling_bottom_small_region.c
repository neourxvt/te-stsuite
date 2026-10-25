#include "te-stsuite.h"

/* Restrict scrolling to roughly the top half of the alternate screen. */
int bench_scrolling_bottom_small_region_prepare(BenchData *data,
                                                const BenchContext *context,
                                                char *error,
                                                size_t error_size) {
    (void)error;
    (void)error_size;
    unsigned bottom = context->rows / 2;
    if (bottom == 0) bottom = 1;
    if (!buffer_appendf(&data->setup, "\033[?1049h\033[1;%ur", bottom)) {
        return -1;
    }
    return buffer_append(&data->payload, "y\n", 2) ? 0 : -1;
}
