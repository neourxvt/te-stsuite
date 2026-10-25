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

/* Keep the suite order stable and put terminal startup before PTY workloads. */
const Benchmark te_stsuite_benchmarks[] = {
    {"startup", bench_startup_prepare},
    {"vtebench_cursor_motion", bench_cursor_motion_prepare},
    {"vtebench_dense_cells", bench_dense_cells_prepare},
    {"vtebench_light_cells", bench_light_cells_prepare},
    {"vtebench_medium_cells", bench_medium_cells_prepare},
    {"vtebench_scrolling", bench_scrolling_prepare},
    {"vtebench_scrolling_bottom_region", bench_scrolling_bottom_region_prepare},
    {"vtebench_scrolling_bottom_small_region", bench_scrolling_bottom_small_region_prepare},
    {"vtebench_scrolling_fullscreen", bench_scrolling_fullscreen_prepare},
    {"vtebench_scrolling_top_region", bench_scrolling_top_region_prepare},
    {"vtebench_scrolling_top_small_region", bench_scrolling_top_small_region_prepare},
    {"vtebench_sync_medium_cells", bench_sync_medium_cells_prepare},
    {"vtebench_unicode", bench_unicode_prepare},
    {"vtebench_missing", bench_missing_prepare},
};

const size_t te_stsuite_benchmark_count =
    sizeof(te_stsuite_benchmarks) / sizeof(te_stsuite_benchmarks[0]);
