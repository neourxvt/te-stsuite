#include "cvtebench.h"

/* Scroll only the lower half of the alternate screen. */
int bench_scrolling_top_small_region_prepare(BenchData *data,
                                             const BenchContext *context,
                                             char *error, size_t error_size) {
    (void)error;
    (void)error_size;
    unsigned top = context->rows / 2;
    if (top == 0) top = 1;
    if (!buffer_appendf(&data->setup, "\033[?1049h\033[%u;%ur", top,
                        context->rows)) {
        return -1;
    }
    return buffer_append(&data->payload, "y\n", 2) ? 0 : -1;
}
