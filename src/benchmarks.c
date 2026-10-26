#include "te-stsuite.h"

#include <string.h>

/* Keep selection and latency reporting consistent for named Typometer runs. */
bool benchmark_is_typometer(const char *name) {
    return strcmp(name, "typometer") == 0 ||
           strncmp(name, "typometer/", sizeof("typometer/") - 1) == 0;
}

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
int bench_termbench_many_line_prepare(BenchData *, const BenchContext *, char *, size_t);
int bench_termbench_long_line_prepare(BenchData *, const BenchContext *, char *, size_t);
int bench_termbench_fg_per_char_prepare(BenchData *, const BenchContext *, char *, size_t);
int bench_termbench_fgbg_per_char_prepare(BenchData *, const BenchContext *, char *, size_t);

bool bench_typometer_execute(const BenchContext *, const RunOptions *, bool,
                              BenchBegin, void *, BenchSamples *, char *, size_t);
bool bench_cell_dimensions_execute(const BenchContext *, const RunOptions *, bool,
                                    BenchBegin, void *, BenchSamples *, char *, size_t);

/* Keep the suite order stable and put terminal startup before PTY workloads. */
const Benchmark te_stsuite_benchmarks[] = {
    {"startup", bench_startup_prepare, NULL, false},
    {"vtebench_cursor_motion", bench_cursor_motion_prepare, NULL, false},
    {"vtebench_dense_cells", bench_dense_cells_prepare, NULL, false},
    {"vtebench_light_cells", bench_light_cells_prepare, NULL, false},
    {"vtebench_medium_cells", bench_medium_cells_prepare, NULL, false},
    {"vtebench_scrolling", bench_scrolling_prepare, NULL, false},
    {"vtebench_scrolling_bottom_region", bench_scrolling_bottom_region_prepare, NULL, false},
    {"vtebench_scrolling_bottom_small_region", bench_scrolling_bottom_small_region_prepare, NULL, false},
    {"vtebench_scrolling_fullscreen", bench_scrolling_fullscreen_prepare, NULL, false},
    {"vtebench_scrolling_top_region", bench_scrolling_top_region_prepare, NULL, false},
    {"vtebench_scrolling_top_small_region", bench_scrolling_top_small_region_prepare, NULL, false},
    {"vtebench_sync_medium_cells", bench_sync_medium_cells_prepare, NULL, false},
    {"vtebench_unicode", bench_unicode_prepare, NULL, false},
    {"vtebench_missing", bench_missing_prepare, NULL, false},
    {"termbench_many_line", bench_termbench_many_line_prepare, NULL, false},
    {"termbench_long_line", bench_termbench_long_line_prepare, NULL, false},
    {"termbench_fg_per_char", bench_termbench_fg_per_char_prepare, NULL, false},
    {"termbench_fgbg_per_char", bench_termbench_fgbg_per_char_prepare, NULL, false},
    {"typometer", NULL, bench_typometer_execute, false},
    {"cell_dimensions", NULL, bench_cell_dimensions_execute, true},
};

const size_t te_stsuite_benchmark_count =
    sizeof(te_stsuite_benchmarks) / sizeof(te_stsuite_benchmarks[0]);
