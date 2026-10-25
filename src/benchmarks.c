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
    {"vtebench-cursor_motion", bench_cursor_motion_prepare},
    {"vtebench-dense_cells", bench_dense_cells_prepare},
    {"vtebench-light_cells", bench_light_cells_prepare},
    {"vtebench-medium_cells", bench_medium_cells_prepare},
    {"vtebench-scrolling", bench_scrolling_prepare},
    {"vtebench-scrolling_bottom_region", bench_scrolling_bottom_region_prepare},
    {"vtebench-scrolling_bottom_small_region", bench_scrolling_bottom_small_region_prepare},
    {"vtebench-scrolling_fullscreen", bench_scrolling_fullscreen_prepare},
    {"vtebench-scrolling_top_region", bench_scrolling_top_region_prepare},
    {"vtebench-scrolling_top_small_region", bench_scrolling_top_small_region_prepare},
    {"vtebench-sync_medium_cells", bench_sync_medium_cells_prepare},
    {"vtebench-unicode", bench_unicode_prepare},
    {"vtebench-missing", bench_missing_prepare},
};

const size_t te_stsuite_benchmark_count =
    sizeof(te_stsuite_benchmarks) / sizeof(te_stsuite_benchmarks[0]);
