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

/* A benchmark definition; startup is represented by a zero-payload entry. */
typedef struct {
    const char *name;
    BenchPrepare prepare;
} Benchmark;

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
} RunOptions;

/* One configured terminal command from the TOML file. */
typedef struct {
    char *name;
    char *command;
    char *inline_config;
    size_t inline_config_length;
} TerminalConfig;

/* The complete parsed configuration, including its source path. */
typedef struct {
    char *path;
    RunOptions options;
    MonitorBackend monitor_backend;
    TerminalConfig *terminals;
    size_t terminal_count;
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

/* Buffer operations keep benchmark generation independent of the runner. */
void buffer_init(Buffer *buffer);
void buffer_free(Buffer *buffer);
bool buffer_append(Buffer *buffer, const void *data, size_t length);
bool buffer_append_char(Buffer *buffer, unsigned char value);
bool buffer_append_repeat(Buffer *buffer, unsigned char value, size_t count);
bool buffer_appendf(Buffer *buffer, const char *format, ...);
bool buffer_load_file(Buffer *buffer, const char *path);
bool buffer_repeat_to_minimum(Buffer *buffer, size_t minimum);
bool benchmark_data_path(char *path, size_t path_size, const char *filename);

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
