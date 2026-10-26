#ifndef TE_STSUITE_H
#define TE_STSUITE_H

#include "monitor.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

/* A growable byte buffer shared by every C benchmark module. */
typedef struct {
    unsigned char *data;
    size_t length;
    size_t capacity;
} Buffer;

/* Terminal dimensions used while preparing dimension-sensitive payloads. */
typedef struct {
    unsigned rows;
    unsigned columns;
} BenchContext;

/* Setup bytes run once per sample and the repeatable benchmark payload. */
typedef struct {
    Buffer setup;
    Buffer payload;
} BenchData;

typedef int (*BenchPrepare)(BenchData *data, const BenchContext *context,
                            char *error, size_t error_size);

/* Runtime options parsed from TOML and shared with the in-terminal worker. */
typedef struct {
    size_t min_bytes;
    unsigned warmup;
    unsigned max_seconds;
    unsigned max_samples;
    /* Startup calibration targets measured duration and number of launches. */
    unsigned startup_min_seconds;
    unsigned startup_min_samples;
    unsigned startup_timeout_seconds;
    /* Typometer counts characters and can queue typing ahead of rendering. */
    unsigned typometer_count;
    unsigned typometer_delay_ms;
    unsigned typometer_pause_every;
    unsigned typometer_pause_ms;
    bool typometer_asynchronous;
} RunOptions;

/* Active benchmarks return timing samples and optional measured properties. */
typedef struct {
    uint64_t *values;
    unsigned count;
    /* Pixel bounds can be reported independently of timing and resources. */
    bool has_cell_dimensions;
    unsigned cell_width_px;
    unsigned cell_height_px;
} BenchSamples;

/* Begin resource accounting after calibration and before the actual workload. */
typedef bool (*BenchBegin)(void *user);
typedef bool (*BenchExecute)(const BenchContext *context,
                             const RunOptions *options, bool warmup,
                             BenchBegin begin, void *user, BenchSamples *samples,
                             char *error, size_t error_size);

/* Payload benchmarks prepare bytes; active benchmarks execute their own loop. */
typedef struct {
    const char *name;
    BenchPrepare prepare;
    BenchExecute execute;
    /* Opt out of interval CPU, memory, and wall-time collection for this workload. */
    bool skip_resource_collection;
} Benchmark;

/* One configured run, with a distinct name and independent runtime settings. */
typedef struct {
    const Benchmark *benchmark;
    char *name;
    RunOptions options;
} BenchmarkRun;

/* One environment override applied only to the launched terminal. */
typedef struct {
    char *name;
    char *value;
} TerminalEnv;

/* One configured terminal command from the TOML file. */
typedef struct {
    char *name;
    char *emulator_name;
    char *command;
    char *version_command;
    unsigned version_head;
    bool has_version_head;
    /* Suppress emulator output during launches; version probes stay visible. */
    bool silence_output;
    char *inline_config;
    size_t inline_config_length;
    TerminalEnv *env;
    size_t env_count;
    /* NULL leaves XENVIRONMENT unchanged; an empty string creates an empty file. */
    char *xresources;
    size_t xresources_length;
} TerminalConfig;

/* The complete parsed configuration, including its source path. */
typedef struct {
    char *path;
    RunOptions options;
    MonitorBackend monitor_backend;
    ProcessIsolationMode process_isolation;
    TerminalConfig *terminals;
    size_t terminal_count;
    BenchmarkRun *benchmarks;
    size_t benchmark_count;
} AppConfig;

/* The worker phase is passed privately by the controller to each terminal. */
typedef enum {
    WORKER_PHASE_CHECK,
    WORKER_PHASE_WARMUP,
    WORKER_PHASE_STARTUP,
    WORKER_PHASE_BENCHMARK,
} WorkerPhase;

/* Optional command-line filters and private in-terminal worker settings. */
typedef struct {
    const char *const *terminal_names;
    size_t terminal_count;
    const char *const *benchmark_names;
    size_t benchmark_count;
    WorkerPhase phase;
    bool worker_cgroup_moved;
} RunSelection;

extern const Benchmark te_stsuite_benchmarks[];
extern const size_t te_stsuite_benchmark_count;

/* Recognize the default Typometer run and its named configuration results. */
bool benchmark_is_typometer(const char *name);

/* Benchmark payloads generated from the source fixtures during the build. */
extern const unsigned char te_stsuite_unicode_symbols[];
extern const size_t te_stsuite_unicode_symbols_length;
extern const unsigned char te_stsuite_medium_cells[];
extern const size_t te_stsuite_medium_cells_length;
extern const unsigned char te_stsuite_sync_medium_cells[];
extern const size_t te_stsuite_sync_medium_cells_length;

/* Buffer operations keep benchmark generation independent of the runner. */
void buffer_init(Buffer *buffer);
void buffer_free(Buffer *buffer);
bool buffer_append(Buffer *buffer, const void *data, size_t length);
bool buffer_append_char(Buffer *buffer, unsigned char value);
bool buffer_append_repeat(Buffer *buffer, unsigned char value, size_t count);
bool buffer_appendf(Buffer *buffer, const char *format, ...);
bool buffer_repeat_to_minimum(Buffer *buffer, size_t minimum);

/* Configuration and execution entry points. */
bool config_load(const char *path, AppConfig *config, char *error,
                 size_t error_size);
void config_free(AppConfig *config);
bool selection_validate(const AppConfig *config, const RunSelection *selection,
                        char *error, size_t error_size);
bool selection_includes_benchmark(const RunSelection *selection,
                                  const char *name);
int worker_run(const AppConfig *config, const RunSelection *selection,
               const char *control_path);
int controller_run(const AppConfig *config, const RunSelection *selection,
                   const char *executable_path);

#endif
