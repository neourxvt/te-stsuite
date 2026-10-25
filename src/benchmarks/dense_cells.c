#include "cvtebench.h"

/* Paint every visible cell with changing bold, italic, underline, and colors. */
int bench_dense_cells_prepare(BenchData *data, const BenchContext *context,
                              char *error, size_t error_size) {
    (void)error;
    (void)error_size;
    unsigned offset = 0;

    for (unsigned letter = 0; letter < 26; ++letter) {
        if (!buffer_append(&data->payload, "\033[H", 3)) return -1;
        for (unsigned row = 1; row <= context->rows; ++row) {
            for (unsigned column = 1; column <= context->columns; ++column) {
                unsigned index = row + column + offset;
                unsigned foreground = index % 156 + 100;
                unsigned background = 255 - index % 156 + 100;
                if (!buffer_appendf(&data->payload,
                                    "\033[38;5;%u;48;5;%u;1;3;4m%c",
                                    foreground, background,
                                    'A' + (int)letter)) return -1;
            }
        }
        ++offset;
    }
    return 0;
}
