#include "cvtebench.h"

/* Fill the scrollback with the same 100001 lines used by the shell setup. */
int bench_scrolling_prepare(BenchData *data, const BenchContext *context,
                            char *error, size_t error_size) {
    (void)context;
    (void)error;
    (void)error_size;
    for (unsigned line = 0; line <= 100000; ++line) {
        if (!buffer_append(&data->setup, "y\n", 2)) return -1;
    }
    return buffer_append(&data->payload, "y\n", 2) ? 0 : -1;
}
