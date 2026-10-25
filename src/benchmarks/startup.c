#include "cvtebench.h"

/* The controller measures startup from exec until the worker reports ready. */
int bench_startup_prepare(BenchData *data, const BenchContext *context,
                          char *error, size_t error_size) {
    (void)data;
    (void)context;
    (void)error;
    (void)error_size;
    return 0;
}
