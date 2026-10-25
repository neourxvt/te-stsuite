#include "cvtebench.h"

/* Scroll filled rows using each alphabetic glyph as the original script did. */
int bench_scrolling_fullscreen_prepare(BenchData *data,
                                      const BenchContext *context,
                                      char *error, size_t error_size) {
    (void)error;
    (void)error_size;
    for (unsigned line = 0; line <= 100000; ++line) {
        if (!buffer_append(&data->setup, "y\n", 2)) return -1;
    }
    for (unsigned letter = 0; letter < 26; ++letter) {
        if (!buffer_append_repeat(&data->payload, 'A' + letter,
                                 context->columns) ||
            !buffer_append_char(&data->payload, '\n')) {
            return -1;
        }
    }
    return 0;
}
