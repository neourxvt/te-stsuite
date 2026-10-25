#include "te-stsuite.h"

#include <stdio.h>

/* Replay the bundled Unicode symbol stream used by vtebench. */
int bench_unicode_prepare(BenchData *data, const BenchContext *context,
                          char *error, size_t error_size) {
    (void)context;
    if (!buffer_append(&data->setup, "\033[?1049h", 8) ||
        !buffer_append(&data->payload, te_stsuite_unicode_symbols,
                       te_stsuite_unicode_symbols_length)) {
        snprintf(error, error_size,
                 "cannot load embedded Unicode benchmark data");
        return -1;
    }
    return 0;
}
