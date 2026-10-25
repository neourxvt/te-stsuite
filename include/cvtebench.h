#ifndef CVTEBENCH_H
#define CVTEBENCH_H

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
    TerminalConfig *terminals;
    size_t terminal_count;
} AppConfig;

/* Optional command-line filters; an empty list means all entries. */
typedef struct {
    const char *const *terminal_names;
    size_t terminal_count;
    const char *const *benchmark_names;
    size_t benchmark_count;
} RunSelection;

extern const Benchmark cvtebench_benchmarks[];
extern const size_t cvtebench_benchmark_count;

/* Buffer operations keep benchmark generation independent of the runner. */
void buffer_init(Buffer *buffer);
void buffer_free(Buffer *buffer);
bool buffer_append(Buffer *buffer, const void *data, size_t length);
bool buffer_append_char(Buffer *buffer, unsigned char value);
bool buffer_append_repeat(Buffer *buffer, unsigned char value, size_t count);
bool buffer_appendf(Buffer *buffer, const char *format, ...);
bool buffer_load_file(Buffer *buffer, const char *path);
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
               const char *socket_path);
int controller_run(const AppConfig *config, const RunSelection *selection,
                   const char *executable_path);

/* Linux process-tree accounting used for per-interval resource summaries. */
typedef struct {
    double wall_seconds;
    double cpu_seconds;
    double average_rss_bytes;
    uint64_t peak_rss_bytes;
} ResourceMetrics;

typedef struct ProcessMonitor ProcessMonitor;
ProcessMonitor *monitor_create(pid_t root_pid, pid_t worker_pid);
void monitor_destroy(ProcessMonitor *monitor);
void monitor_set_worker(ProcessMonitor *monitor, pid_t worker_pid,
                        pid_t terminal_pid);
void monitor_begin(ProcessMonitor *monitor, uint64_t start_ns);
void monitor_tick(ProcessMonitor *monitor, uint64_t now_ns);
ResourceMetrics monitor_end(ProcessMonitor *monitor, uint64_t end_ns);
uint64_t monotonic_now_ns(void);

#endif
