#include "te-stsuite.h"

int bench_startup_prepare(BenchData *, const BenchContext *, char *, size_t);
int bench_cursor_motion_prepare(BenchData *, const BenchContext *, char *, size_t);
int bench_dense_cells_prepare(BenchData *, const BenchContext *, char *, size_t);
int bench_light_cells_prepare(BenchData *, const BenchContext *, char *, size_t);
int bench_medium_cells_prepare(BenchData *, const BenchContext *, char *, size_t);
int bench_scrolling_prepare(BenchData *, const BenchContext *, char *, size_t);
int bench_scrolling_bottom_region_prepare(BenchData *, const BenchContext *, char *, size_t);
int bench_scrolling_bottom_small_region_prepare(BenchData *, const BenchContext *, char *, size_t);
int bench_scrolling_fullscreen_prepare(BenchData *, const BenchContext *, char *, size_t);
int bench_scrolling_top_region_prepare(BenchData *, const BenchContext *, char *, size_t);
int bench_scrolling_top_small_region_prepare(BenchData *, const BenchContext *, char *, size_t);
int bench_sync_medium_cells_prepare(BenchData *, const BenchContext *, char *, size_t);
int bench_unicode_prepare(BenchData *, const BenchContext *, char *, size_t);
int bench_missing_prepare(BenchData *, const BenchContext *, char *, size_t);

bool bench_typometer_execute(const BenchContext *, const RunOptions *, bool,
                              BenchBegin, void *, BenchSamples *, char *, size_t);

/* Keep the suite order stable and put terminal startup before PTY workloads. */
const Benchmark te_stsuite_benchmarks[] = {
    {"startup", bench_startup_prepare, NULL},
    {"vtebench_cursor_motion", bench_cursor_motion_prepare, NULL},
    {"vtebench_dense_cells", bench_dense_cells_prepare, NULL},
    {"vtebench_light_cells", bench_light_cells_prepare, NULL},
    {"vtebench_medium_cells", bench_medium_cells_prepare, NULL},
    {"vtebench_scrolling", bench_scrolling_prepare, NULL},
    {"vtebench_scrolling_bottom_region", bench_scrolling_bottom_region_prepare, NULL},
    {"vtebench_scrolling_bottom_small_region", bench_scrolling_bottom_small_region_prepare, NULL},
    {"vtebench_scrolling_fullscreen", bench_scrolling_fullscreen_prepare, NULL},
    {"vtebench_scrolling_top_region", bench_scrolling_top_region_prepare, NULL},
    {"vtebench_scrolling_top_small_region", bench_scrolling_top_small_region_prepare, NULL},
    {"vtebench_sync_medium_cells", bench_sync_medium_cells_prepare, NULL},
    {"vtebench_unicode", bench_unicode_prepare, NULL},
    {"vtebench_missing", bench_missing_prepare, NULL},
    {"typometer", NULL, bench_typometer_execute},
};

const size_t te_stsuite_benchmark_count =
    sizeof(te_stsuite_benchmarks) / sizeof(te_stsuite_benchmarks[0]);
