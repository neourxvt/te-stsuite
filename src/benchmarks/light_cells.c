#include "cvtebench.h"

/* Repaint the visible grid with plain single-width characters. */
int bench_light_cells_prepare(BenchData *data, const BenchContext *context,
                              char *error, size_t error_size) {
    (void)error;
    (void)error_size;
    size_t cells = (size_t)context->rows * context->columns;

    for (unsigned letter = 0; letter < 26; ++letter) {
        if (!buffer_append(&data->payload, "\033[H", 3) ||
            !buffer_append_repeat(&data->payload, 'A' + letter, cells)) {
            return -1;
        }
    }
    return 0;
}
