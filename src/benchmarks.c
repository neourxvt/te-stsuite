#include "cvtebench.h"

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
const Benchmark cvtebench_benchmarks[] = {
    {"startup", bench_startup_prepare},
    {"cursor_motion", bench_cursor_motion_prepare},
    {"dense_cells", bench_dense_cells_prepare},
    {"light_cells", bench_light_cells_prepare},
    {"medium_cells", bench_medium_cells_prepare},
    {"scrolling", bench_scrolling_prepare},
    {"scrolling_bottom_region", bench_scrolling_bottom_region_prepare},
    {"scrolling_bottom_small_region", bench_scrolling_bottom_small_region_prepare},
    {"scrolling_fullscreen", bench_scrolling_fullscreen_prepare},
    {"scrolling_top_region", bench_scrolling_top_region_prepare},
    {"scrolling_top_small_region", bench_scrolling_top_small_region_prepare},
    {"sync_medium_cells", bench_sync_medium_cells_prepare},
    {"unicode", bench_unicode_prepare},
    {"missing", bench_missing_prepare},
};

const size_t cvtebench_benchmark_count =
    sizeof(cvtebench_benchmarks) / sizeof(cvtebench_benchmarks[0]);
