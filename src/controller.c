#define _GNU_SOURCE
#include "te-stsuite.h"
#include "process_isolation.h"
#include "yyjson.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

typedef struct {
    char **items;
    size_t count;
    size_t capacity;
} StringList;

typedef struct {
    char terminal[128];
    char benchmark[128];
    size_t bytes;
    unsigned samples;
    double sample_mean_ms;
    double sample_p90_ms;
    double sample_stddev_ms;
    MonitorMetrics metrics;
    bool complete;
} Summary;

/* One independently launched terminal sample for the startup benchmark. */
typedef struct {
    uint64_t duration_ns;
    MonitorMetrics metrics;
} StartupSample;

typedef struct {
    Summary *items;
    size_t count;
    size_t capacity;
} SummaryList;

/* Preserve each terminal's isolation result for the all-terminal check report. */
typedef struct {
    const char *terminal;
    MonitorIsolation isolation;
    char detail[512];
    bool recorded;
    bool passed;
    bool worker_moved;
} CgroupCheckResult;

static FILE *log_file;
static FILE *jsonl_file;
static size_t jsonl_result_count;
static bool jsonl_failed;
static bool color_enabled;

static const char *const color_reset = "\033[0m";
static const char *const color_bold_cyan = "\033[1;36m";
static const char *const color_cyan = "\033[36m";
static const char *const color_green = "\033[32m";
static const char *const color_yellow = "\033[33m";
static const char *const color_red = "\033[31m";
static const char *const color_dim = "\033[2m";

/* Match a terminal name directly, or its base name across config variants. */
static bool terminal_name_matches(const char *configured, const char *requested) {
    size_t length = strlen(requested);
    return strcmp(configured, requested) == 0 ||
           (strncmp(configured, requested, length) == 0 &&
            configured[length] == '/');
}

/* Apply the same terminal selector rules to each of the three run phases. */
static bool terminal_is_selected(const TerminalConfig *terminal,
                                const RunSelection *selection) {
    if (selection == NULL || selection->terminal_count == 0) return true;
    for (size_t i = 0; i < selection->terminal_count; ++i) {
        if (terminal_name_matches(terminal->name,
                                  selection->terminal_names[i])) return true;
    }
    return false;
}

/* Check a supplied selector set before opening a log or launching terminals. */
bool selection_validate(const AppConfig *config, const RunSelection *selection,
                        char *error, size_t error_size) {
    if (selection == NULL) return true;
    for (size_t i = 0; i < selection->terminal_count; ++i) {
        bool found = false;
        for (size_t j = 0; j < config->terminal_count; ++j) {
            if (terminal_name_matches(config->terminals[j].name,
                                      selection->terminal_names[i])) {
                found = true;
                break;
            }
        }
        if (!found) {
            snprintf(error, error_size, "unknown terminal selector '%s'",
                     selection->terminal_names[i]);
            return false;
        }
    }
    for (size_t i = 0; i < selection->benchmark_count; ++i) {
        bool found = false;
        for (size_t j = 0; j < te_stsuite_benchmark_count; ++j) {
            if (strcmp(te_stsuite_benchmarks[j].name,
                       selection->benchmark_names[i]) == 0) {
                found = true;
                break;
            }
        }
        if (!found) {
            snprintf(error, error_size, "unknown benchmark '%s'",
                     selection->benchmark_names[i]);
            return false;
        }
    }
    return true;
}

/* Return true when a benchmark is selected, with an empty list meaning all. */
bool selection_includes_benchmark(const RunSelection *selection,
                                  const char *name) {
    if (selection == NULL || selection->benchmark_count == 0) return true;
    for (size_t i = 0; i < selection->benchmark_count; ++i) {
        if (strcmp(selection->benchmark_names[i], name) == 0) return true;
    }
    return false;
}

/* Count selected workloads so warmup is skipped when no measurement will run. */
static size_t selected_benchmark_count(const RunSelection *selection) {
    size_t count = 0;
    for (size_t i = 0; i < te_stsuite_benchmark_count; ++i) {
        if (selection_includes_benchmark(selection,
                                         te_stsuite_benchmarks[i].name)) {
            count++;
        }
    }
    return count;
}

/* Keep terminal styling out of the plain-text log while sharing formatting. */
static void vreport(const char *color, const char *format, va_list args) {
    if (color_enabled && color != NULL) fputs(color, stdout);
    va_list console_args;
    va_copy(console_args, args);
    vfprintf(stdout, format, console_args);
    va_end(console_args);
    if (color_enabled && color != NULL) fputs(color_reset, stdout);

    if (log_file != NULL) {
        va_list log_args;
        va_copy(log_args, args);
        vfprintf(log_file, format, log_args);
        va_end(log_args);
    }
    fflush(stdout);
    if (log_file != NULL) fflush(log_file);
}

/* Write one unstyled message to the console and run log. */
static void report(const char *format, ...) {
    va_list args;
    va_start(args, format);
    vreport(NULL, format, args);
    va_end(args);
}

/* Write a colorized console message and a plain run-log message. */
static void report_styled(const char *color, const char *format, ...) {
    va_list args;
    va_start(args, format);
    vreport(color, format, args);
    va_end(args);
}

/* Append one owned word to a dynamically sized command token list. */
static bool string_list_push(StringList *list, const char *word, size_t length) {
    if (list->count == list->capacity) {
        size_t capacity = list->capacity == 0 ? 8 : list->capacity * 2;
        char **items = realloc(list->items, capacity * sizeof(*items));
        if (items == NULL) return false;
        list->items = items;
        list->capacity = capacity;
    }
    char *copy = malloc(length + 1);
    if (copy == NULL) return false;
    memcpy(copy, word, length);
    copy[length] = '\0';
    list->items[list->count++] = copy;
    return true;
}

/* Split a command template using simple shell-style quote and escape rules. */
static bool split_command(const char *command, StringList *words,
                          char *error, size_t error_size) {
    Buffer word;
    buffer_init(&word);
    char quote = '\0';
    bool escaped = false;
    bool started = false;

    for (const char *cursor = command; ; ++cursor) {
        char value = *cursor;
        if (escaped) {
            if (value == '\0' || !buffer_append_char(&word, (unsigned char)value)) {
                snprintf(error, error_size, "invalid escape or out of memory in terminal command");
                buffer_free(&word);
                return false;
            }
            escaped = false;
            started = true;
            continue;
        }
        if (quote == '\'' && value != '\0') {
            if (value == '\'') quote = '\0';
            else if (!buffer_append_char(&word, (unsigned char)value)) goto memory_error;
            started = true;
            continue;
        }
        if (quote == '"' && value != '\0') {
            if (value == '"') quote = '\0';
            else if (value == '\\') escaped = true;
            else if (!buffer_append_char(&word, (unsigned char)value)) goto memory_error;
            started = true;
            continue;
        }
        if (value == '\0' || value == ' ' || value == '\t' ||
            value == '\n' || value == '\r') {
            if (quote != '\0') {
                snprintf(error, error_size, "unclosed quote in terminal command template");
                buffer_free(&word);
                return false;
            }
            if (started) {
                if (!string_list_push(words, (char *)word.data, word.length)) {
                    goto memory_error;
                }
                word.length = 0;
                started = false;
            }
            if (value == '\0') break;
            continue;
        }
        if (value == '\'' || value == '"') {
            quote = value;
            started = true;
        } else if (value == '\\') {
            escaped = true;
            started = true;
        } else {
            if (!buffer_append_char(&word, (unsigned char)value)) goto memory_error;
            started = true;
        }
    }
    buffer_free(&word);
    if (words->count == 0) {
        snprintf(error, error_size, "terminal command template is empty");
        return false;
    }
    return true;

memory_error:
    snprintf(error, error_size, "out of memory while parsing terminal command");
    buffer_free(&word);
    return false;
}

/* Release token strings allocated for one terminal command. */
static void string_list_free(StringList *list) {
    for (size_t i = 0; i < list->count; ++i) free(list->items[i]);
    free(list->items);
    memset(list, 0, sizeof(*list));
}

/* Build argv by replacing worker and optional temporary-config placeholders. */
static char **expand_command(const TerminalConfig *terminal,
                             const AppConfig *config,
                             const RunSelection *selection,
                             const char *executable_path,
                             const char *control_path, const char *config_path,
                             WorkerPhase phase,
                             const char *worker_cgroup_path,
                             size_t *argc_out,
                             char *error, size_t error_size) {
    StringList words = {0};
    if (!split_command(terminal->command, &words, error, error_size)) {
        string_list_free(&words);
        return NULL;
    }

    size_t command_placeholders = 0;
    size_t config_placeholders = 0;
    for (size_t i = 0; i < words.count; ++i) {
        if (strcmp(words.items[i], "{CMD}") == 0) ++command_placeholders;
        else if (strstr(words.items[i], "{CMD}") != NULL) {
            snprintf(error, error_size,
                     "{CMD} must be a standalone word in terminal command '%s'",
                     terminal->name);
            string_list_free(&words);
            return NULL;
        }
        if (strcmp(words.items[i], "{CFG}") == 0) ++config_placeholders;
        else if (strstr(words.items[i], "{CFG}") != NULL) {
            snprintf(error, error_size,
                     "{CFG} must be a standalone word in terminal command '%s'",
                     terminal->name);
            string_list_free(&words);
            return NULL;
        }
    }
    if (command_placeholders != 1) {
        snprintf(error, error_size,
                 "terminal command '%s' must contain {CMD} exactly once",
                 terminal->name);
        string_list_free(&words);
        return NULL;
    }
    if ((terminal->inline_config != NULL && config_placeholders != 1) ||
        (terminal->inline_config == NULL && config_placeholders != 0)) {
        snprintf(error, error_size,
                 "terminal command '%s' must contain {CFG} exactly once when configs are defined, and never otherwise",
                 terminal->name);
        string_list_free(&words);
        return NULL;
    }

    size_t selected_count = selection == NULL ? 0 : selection->benchmark_count;
    size_t argc = words.count + 8 + selected_count * 2;
    char **argv = calloc(argc + 1, sizeof(*argv));
    if (argv == NULL) {
        snprintf(error, error_size, "out of memory while expanding terminal command");
        string_list_free(&words);
        return NULL;
    }
    size_t output = 0;
    for (size_t i = 0; i < words.count; ++i) {
        if (strcmp(words.items[i], "{CFG}") == 0) {
            argv[output++] = strdup(config_path);
        } else if (strcmp(words.items[i], "{CMD}") != 0) {
            argv[output++] = strdup(words.items[i]);
        } else {
            argv[output++] = strdup(executable_path);
            argv[output++] = strdup(config->path);
            argv[output++] = strdup("--run");
            argv[output++] = strdup("--worker-control");
            argv[output++] = strdup(control_path);
            argv[output++] = strdup("--worker-phase");
            argv[output++] = strdup(phase == WORKER_PHASE_CHECK
                                        ? "check"
                                        : phase == WORKER_PHASE_WARMUP
                                              ? "warmup"
                                              : phase == WORKER_PHASE_STARTUP
                                                    ? "startup"
                                                    : "benchmark");
            argv[output++] = strdup("--worker-cgroup");
            argv[output++] = strdup(worker_cgroup_path);
        }
    }
    for (size_t i = 0; i < selected_count; ++i) {
        argv[output++] = strdup("--benchmark");
        argv[output++] = strdup(selection->benchmark_names[i]);
    }
    argv[output] = NULL;
    *argc_out = output;
    string_list_free(&words);
    for (size_t i = 0; i < output; ++i) {
        if (argv[i] == NULL) {
            snprintf(error, error_size, "out of memory while expanding terminal command");
            for (size_t j = 0; j < output; ++j) free(argv[j]);
            free(argv);
            return NULL;
        }
    }
    return argv;
}

/* Write private text content to a temporary file for one terminal launch. */
static bool write_temporary_file(const char *content, size_t content_length,
                                 const char *path_template, const char *kind,
                                 char *path, size_t path_size, char *error,
                                 size_t error_size) {
    if (snprintf(path, path_size, "%s", path_template) >= (int)path_size) {
        snprintf(error, error_size, "temporary %s path buffer is too small",
                 kind);
        return false;
    }
    int fd = mkstemp(path);
    if (fd < 0) {
        snprintf(error, error_size, "cannot create temporary %s: %s", kind,
                 strerror(errno));
        return false;
    }
    size_t written = 0;
    while (written < content_length) {
        ssize_t count = write(fd, content + written,
                              content_length - written);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) {
            snprintf(error, error_size, "cannot write temporary %s: %s", kind,
                     count < 0 ? strerror(errno) : "short write");
            close(fd);
            unlink(path);
            return false;
        }
        written += (size_t)count;
    }
    if (close(fd) != 0) {
        snprintf(error, error_size, "cannot close temporary %s: %s", kind,
                 strerror(errno));
        unlink(path);
        return false;
    }
    return true;
}

/* Create a private config file because terminal CLI options need a path. */
static bool write_inline_config(const TerminalConfig *terminal, char *path,
                                size_t path_size, char *error,
                                size_t error_size) {
    if (terminal->inline_config == NULL) {
        path[0] = '\0';
        return true;
    }
    return write_temporary_file(
        terminal->inline_config, terminal->inline_config_length,
        "/tmp/te-stsuite-config-XXXXXX", "config", path, path_size,
        error, error_size);
}

/* Create per-run X resources consumed through the XENVIRONMENT variable. */
static bool write_xresources(const TerminalConfig *terminal, char *path,
                             size_t path_size, char *error,
                             size_t error_size) {
    if (terminal->xresources == NULL) {
        path[0] = '\0';
        return true;
    }
    return write_temporary_file(
        terminal->xresources, terminal->xresources_length,
        "/tmp/te-stsuite-xresources-XXXXXX", "X resources", path,
        path_size, error, error_size);
}

/* Remove temporary files after every setup failure and completed launch. */
static void cleanup_terminal_files(const char *config_path,
                                   const char *xresources_path) {
    if (config_path[0] != '\0') unlink(config_path);
    if (xresources_path[0] != '\0') unlink(xresources_path);
}

/* Release an expanded argv vector after the forked child has copied it. */
static void free_argv(char **argv, size_t argc) {
    for (size_t i = 0; i < argc; ++i) free(argv[i]);
    free(argv);
}

/* Add one completed benchmark result to the collective summary list. */
static bool summary_push(SummaryList *list, const Summary *summary) {
    if (list->count == list->capacity) {
        size_t capacity = list->capacity == 0 ? 16 : list->capacity * 2;
        Summary *items = realloc(list->items, capacity * sizeof(*items));
        if (items == NULL) return false;
        list->items = items;
        list->capacity = capacity;
    }
    list->items[list->count++] = *summary;
    return true;
}

/* Print one benchmark result with consistently labeled resource measurements. */
static void print_summary(const Summary *summary) {
    double cpu_percent = summary->metrics.wall_seconds > 0
                             ? summary->metrics.cpu_seconds /
                                   summary->metrics.wall_seconds * 100.0
                             : 0;
    const char *result_color = summary->complete ? color_green : color_yellow;
    report_styled(summary->complete ? color_bold_cyan : color_yellow,
                  "  %s%s\n", summary->benchmark,
                  summary->complete ? "" : "  [PARTIAL]");
    if (summary->metrics.resources_available) {
        report_styled(result_color,
                      "    Wall       %9.2f ms   CPU %8.2f%% (%9.2f ms)   Memory avg %8.2f MiB  peak %8.2f MiB   N %u\n",
                      summary->metrics.wall_seconds * 1000.0, cpu_percent,
                      summary->metrics.cpu_seconds * 1000.0,
                      summary->metrics.average_memory_bytes / 1048576.0,
                      (double)summary->metrics.peak_memory_bytes / 1048576.0,
                      summary->samples);
    } else {
        report_styled(result_color,
                      "    Wall       %9.2f ms   CPU      n/a (      n/a ms)   Memory avg      n/a  peak      n/a   N %u\n",
                      summary->metrics.wall_seconds * 1000.0,
                      summary->samples);
    }
    if (summary->bytes > 0) {
        report_styled(color_dim,
                      "    PTY write  avg %9.2f ms   p90 %9.2f ms   sd %9.2f ms   payload %zu B/sample\n",
                      summary->sample_mean_ms, summary->sample_p90_ms,
                      summary->sample_stddev_ms, summary->bytes);
    } else if (strcmp(summary->benchmark, "startup") == 0) {
        report_styled(color_dim,
                      "    Startup    avg %9.2f ms   p90 %9.2f ms   sd %9.2f ms\n",
                      summary->sample_mean_ms, summary->sample_p90_ms,
                      summary->sample_stddev_ms);
    }
    if (!summary->complete) {
        report_styled(color_yellow,
                      "    Result is partial because the benchmark did not finish.\n");
    }
}

/* Extend a table rule in chunks so unusually long configured names stay safe. */
static void print_table_rule(size_t width) {
    static const char dashes[] =
        "------------------------------------------------------------------------------------------------"
        "------------------------------------------------------------------------------------------------";
    while (width > 0) {
        size_t chunk = width < sizeof(dashes) - 1 ? width : sizeof(dashes) - 1;
        report_styled(color_dim, "%.*s", (int)chunk, dashes);
        width -= chunk;
    }
    report("\n");
}

/* Return a terminal or benchmark column width large enough for its values. */
static size_t summary_column_width(const SummaryList *summaries, bool terminal) {
    size_t width = terminal ? strlen("Terminal") : strlen("Benchmark");
    for (size_t i = 0; i < summaries->count; ++i) {
        size_t length = strlen(terminal ? summaries->items[i].terminal
                                        : summaries->items[i].benchmark);
        if (length > width) width = length;
    }
    return width;
}

/* Print aligned resource and PTY timing tables for all collected results. */
static void print_collective_summary(const SummaryList *summaries) {
    size_t terminal_width = summary_column_width(summaries, true);
    size_t benchmark_width = summary_column_width(summaries, false);
    bool has_write_results = false;
    bool has_startup_results = false;
    for (size_t i = 0; i < summaries->count; ++i) {
        if (summaries->items[i].bytes > 0) has_write_results = true;
        if (strcmp(summaries->items[i].benchmark, "startup") == 0) {
            has_startup_results = true;
        }
    }

    report_styled(color_bold_cyan, "Collective results\n");
    size_t resource_width = terminal_width + benchmark_width + 2 * 8 + 10 + 9 +
                            10 + 15 + 15 + 7 + 9;
    report_styled(color_bold_cyan,
                  "%-*s  %-*s  %10s  %9s  %10s  %15s  %15s  %7s  %9s\n",
                  (int)terminal_width, "Terminal", (int)benchmark_width,
                  "Benchmark", "Wall ms", "CPU %", "CPU ms", "Memory avg MiB",
                  "Memory peak MiB", "N", "Result");
    print_table_rule(resource_width);
    for (size_t i = 0; i < summaries->count; ++i) {
        const Summary *summary = &summaries->items[i];
        double cpu_percent = summary->metrics.wall_seconds > 0
                                 ? summary->metrics.cpu_seconds /
                                       summary->metrics.wall_seconds * 100.0
                                 : 0;
        if (summary->metrics.resources_available) {
            report_styled(summary->complete ? color_green : color_yellow,
                          "%-*s  %-*s  %10.2f  %9.2f  %10.2f  %15.2f  %15.2f  %7u  %9s\n",
                          (int)terminal_width, summary->terminal,
                          (int)benchmark_width, summary->benchmark,
                          summary->metrics.wall_seconds * 1000.0, cpu_percent,
                          summary->metrics.cpu_seconds * 1000.0,
                          summary->metrics.average_memory_bytes / 1048576.0,
                          (double)summary->metrics.peak_memory_bytes / 1048576.0,
                          summary->samples,
                          summary->complete ? "complete" : "partial");
        } else {
            report_styled(summary->complete ? color_yellow : color_red,
                          "%-*s  %-*s  %10.2f  %9s  %10s  %15s  %15s  %7u  %9s\n",
                          (int)terminal_width, summary->terminal,
                          (int)benchmark_width, summary->benchmark,
                          summary->metrics.wall_seconds * 1000.0, "n/a", "n/a",
                          "n/a", "n/a", summary->samples,
                          summary->complete ? "complete" : "partial");
        }
    }

    if (has_startup_results) {
        report("\n");
        report_styled(color_bold_cyan, "Startup results\n");
        size_t startup_width = terminal_width + benchmark_width + 2 * 6 + 12 +
                               12 + 12 + 9;
        report_styled(color_bold_cyan,
                      "%-*s  %-*s  %12s  %12s  %12s  %9s\n",
                      (int)terminal_width, "Terminal",
                      (int)benchmark_width, "Benchmark", "Avg ms", "P90 ms",
                      "SD ms", "Samples");
        print_table_rule(startup_width);
        for (size_t i = 0; i < summaries->count; ++i) {
            const Summary *summary = &summaries->items[i];
            if (strcmp(summary->benchmark, "startup") != 0) continue;
            report_styled(summary->complete ? color_green : color_yellow,
                          "%-*s  %-*s  %12.2f  %12.2f  %12.2f  %9u\n",
                          (int)terminal_width, summary->terminal,
                          (int)benchmark_width, summary->benchmark,
                          summary->sample_mean_ms, summary->sample_p90_ms,
                          summary->sample_stddev_ms, summary->samples);
        }
    }

    if (has_write_results) {
        report("\n");
        report_styled(color_bold_cyan, "PTY write results\n");
        size_t write_width = terminal_width + benchmark_width + 2 * 6 + 13 +
                             12 + 12 + 12 + 9;
        report_styled(color_bold_cyan,
                      "%-*s  %-*s  %13s  %12s  %12s  %12s  %9s\n",
                      (int)terminal_width, "Terminal", (int)benchmark_width,
                      "Benchmark", "Payload B", "Avg ms", "P90 ms", "SD ms",
                      "Samples");
        print_table_rule(write_width);
        for (size_t i = 0; i < summaries->count; ++i) {
            const Summary *summary = &summaries->items[i];
            if (summary->bytes == 0) continue;
            report_styled(summary->complete ? color_green : color_yellow,
                          "%-*s  %-*s  %13zu  %12.2f  %12.2f  %12.2f  %9u\n",
                          (int)terminal_width, summary->terminal,
                          (int)benchmark_width, summary->benchmark,
                          summary->bytes, summary->sample_mean_ms,
                          summary->sample_p90_ms, summary->sample_stddev_ms,
                          summary->samples);
        }
    }
}

/* Report the first JSONL failure while allowing the run log to continue. */
static bool jsonl_error(void) {
    if (!jsonl_failed) {
        int saved_errno = errno;
        fprintf(stderr, "te-stsuite: JSONL logging failed: %s\n",
                saved_errno == 0 ? "serialization error" : strerror(saved_errno));
    }
    jsonl_failed = true;
    return false;
}

/* Serialize one compact object and flush its JSONL line to disk. */
static bool jsonl_write_doc(yyjson_mut_doc *doc, yyjson_mut_val *object) {
    if (doc == NULL || object == NULL || jsonl_file == NULL) {
        if (doc != NULL) yyjson_mut_doc_free(doc);
        errno = ENOMEM;
        return jsonl_error();
    }
    yyjson_mut_doc_set_root(doc, object);
    size_t length = 0;
    char *encoded = yyjson_mut_write(doc, YYJSON_WRITE_NOFLAG, &length);
    if (encoded == NULL) {
        yyjson_mut_doc_free(doc);
        errno = ENOMEM;
        return jsonl_error();
    }
    bool written = fwrite(encoded, 1, length, jsonl_file) == length &&
                   fputc('\n', jsonl_file) != EOF && fflush(jsonl_file) == 0;
    free(encoded);
    yyjson_mut_doc_free(doc);
    if (!written) return jsonl_error();
    return true;
}

/* Allocate an empty object record for the next JSONL line. */
static yyjson_mut_doc *jsonl_new_object(yyjson_mut_val **object) {
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    *object = doc == NULL ? NULL : yyjson_mut_obj(doc);
    if (*object == NULL) {
        if (doc != NULL) yyjson_mut_doc_free(doc);
        errno = ENOMEM;
        jsonl_error();
        return NULL;
    }
    return doc;
}

/* Write run metadata once so result rows stay flat and self-contained. */
static void jsonl_write_run(const AppConfig *config, const char *started_at) {
    yyjson_mut_val *object = NULL;
    yyjson_mut_doc *doc = jsonl_new_object(&object);
    if (doc == NULL) return;
    bool ok = yyjson_mut_obj_add_str(doc, object, "type", "run") &&
              yyjson_mut_obj_add_uint(doc, object, "schema", 2) &&
              yyjson_mut_obj_add_str(doc, object, "started_at", started_at) &&
              yyjson_mut_obj_add_str(doc, object, "config", config->path) &&
              yyjson_mut_obj_add_str(doc, object, "monitor",
                                     monitor_backend_name(config->monitor_backend)) &&
              yyjson_mut_obj_add_str(doc, object, "process_isolation",
                                     process_isolation_name(config->process_isolation));
    if (!ok) {
        yyjson_mut_doc_free(doc);
        errno = ENOMEM;
        jsonl_error();
        return;
    }
    jsonl_write_doc(doc, object);
}

/* Write one unique benchmark measurement row, without repeated summaries. */
static void jsonl_write_result(const Summary *summary) {
    yyjson_mut_val *object = NULL;
    yyjson_mut_doc *doc = jsonl_new_object(&object);
    if (doc == NULL) return;
    double cpu_percent = summary->metrics.wall_seconds > 0
                             ? summary->metrics.cpu_seconds /
                                   summary->metrics.wall_seconds * 100.0
                             : 0;
    bool ok = yyjson_mut_obj_add_str(doc, object, "type", "result") &&
              yyjson_mut_obj_add_str(doc, object, "terminal", summary->terminal) &&
              yyjson_mut_obj_add_str(doc, object, "benchmark", summary->benchmark) &&
              yyjson_mut_obj_add_real(doc, object, "wall_s",
                                      summary->metrics.wall_seconds) &&
              yyjson_mut_obj_add_bool(doc, object, "resources_available",
                                      summary->metrics.resources_available) &&
              yyjson_mut_obj_add_uint(doc, object, "samples", summary->samples) &&
              yyjson_mut_obj_add_uint(doc, object, "payload_bytes",
                                      (uint64_t)summary->bytes) &&
              yyjson_mut_obj_add_bool(doc, object, "complete", summary->complete);
    if (ok && summary->metrics.resources_available) {
        ok = yyjson_mut_obj_add_real(doc, object, "cpu_s",
                                     summary->metrics.cpu_seconds) &&
             yyjson_mut_obj_add_real(doc, object, "cpu_pct", cpu_percent) &&
             yyjson_mut_obj_add_uint(doc, object, "mem_avg_bytes",
                                     (uint64_t)summary->metrics.average_memory_bytes) &&
             yyjson_mut_obj_add_uint(doc, object, "mem_peak_bytes",
                                     summary->metrics.peak_memory_bytes);
    } else if (ok) {
        ok = yyjson_mut_obj_add_null(doc, object, "cpu_s") &&
             yyjson_mut_obj_add_null(doc, object, "cpu_pct") &&
             yyjson_mut_obj_add_null(doc, object, "mem_avg_bytes") &&
             yyjson_mut_obj_add_null(doc, object, "mem_peak_bytes");
    }
    if (ok && summary->bytes > 0) {
        ok = yyjson_mut_obj_add_real(doc, object, "write_avg_ms",
                                     summary->sample_mean_ms) &&
             yyjson_mut_obj_add_real(doc, object, "write_p90_ms",
                                     summary->sample_p90_ms) &&
             yyjson_mut_obj_add_real(doc, object, "write_sd_ms",
                                     summary->sample_stddev_ms);
    } else if (ok && strcmp(summary->benchmark, "startup") == 0) {
        ok = yyjson_mut_obj_add_real(doc, object, "startup_avg_ms",
                                     summary->sample_mean_ms) &&
             yyjson_mut_obj_add_real(doc, object, "startup_p90_ms",
                                     summary->sample_p90_ms) &&
             yyjson_mut_obj_add_real(doc, object, "startup_sd_ms",
                                     summary->sample_stddev_ms);
    }
    if (!ok) {
        yyjson_mut_doc_free(doc);
        errno = ENOMEM;
        jsonl_error();
        return;
    }
    if (jsonl_write_doc(doc, object)) ++jsonl_result_count;
}

/* Record the automatic cgroup placement check without creating a result row. */
static void jsonl_write_cgroup_check(const char *terminal,
                                     MonitorIsolation isolation,
                                     const char *detail,
                                     bool worker_moved) {
    yyjson_mut_val *object = NULL;
    yyjson_mut_doc *doc = jsonl_new_object(&object);
    if (doc == NULL) return;
    const char *status = isolation == MONITOR_ISOLATION_INSIDE
                             ? "inside"
                             : isolation == MONITOR_ISOLATION_OUTSIDE
                                   ? "outside"
                                   : "unavailable";
    bool ok = yyjson_mut_obj_add_str(doc, object, "type", "cgroup_check") &&
              yyjson_mut_obj_add_str(doc, object, "terminal", terminal) &&
              yyjson_mut_obj_add_str(doc, object, "status", status) &&
              yyjson_mut_obj_add_bool(doc, object, "worker_moved", worker_moved);
    if (ok) {
        ok = detail == NULL
                 ? yyjson_mut_obj_add_null(doc, object, "detail")
                 : yyjson_mut_obj_add_str(doc, object, "detail", detail);
    }
    if (!ok) {
        yyjson_mut_doc_free(doc);
        errno = ENOMEM;
        jsonl_error();
        return;
    }
    jsonl_write_doc(doc, object);
}

/* Return a short human-facing status label for the failure summary. */
static const char *cgroup_check_status(MonitorIsolation isolation) {
    switch (isolation) {
        case MONITOR_ISOLATION_INSIDE: return "passed";
        case MONITOR_ISOLATION_OUTSIDE: return "outside";
        case MONITOR_ISOLATION_UNAVAILABLE: return "unavailable";
    }
    return "unavailable";
}

/* Show all selected terminals together when isolation prevents the run. */
static void print_cgroup_check_summary(const CgroupCheckResult *results,
                                      size_t count) {
    size_t terminal_width = strlen("Terminal");
    for (size_t i = 0; i < count; ++i) {
        size_t length = strlen(results[i].terminal);
        if (length > terminal_width) terminal_width = length;
    }
    report_styled(color_bold_cyan, "\nCgroup check summary\n");
    report("%-*s  %-12s  %s\n", (int)terminal_width, "Terminal",
           "Status", "Detail");
    for (size_t i = 0; i < terminal_width + 2 + 12 + 2 + 6; ++i) {
        report("-");
    }
    report("\n");
    for (size_t i = 0; i < count; ++i) {
        const CgroupCheckResult *result = &results[i];
        const char *color = result->passed ? color_green : color_red;
        report_styled(color, "%-*s  %-12s  %s\n",
                      (int)terminal_width, result->terminal,
                      cgroup_check_status(result->isolation),
                      result->detail[0] == '\0' ? "check did not complete"
                                                 : result->detail);
    }
    report("\nRun te-stsuite inside a writable cgroup v2 subtree delegated by\n");
    report("the system or container manager. Do not change permissions on\n");
    report("/sys/fs/cgroup itself.\n");
}

/* Mark the end of a run with success and the number of result rows emitted. */
static void jsonl_write_end(bool success) {
    yyjson_mut_val *object = NULL;
    yyjson_mut_doc *doc = jsonl_new_object(&object);
    if (doc == NULL) return;
    bool ok = yyjson_mut_obj_add_str(doc, object, "type", "end") &&
              yyjson_mut_obj_add_str(doc, object, "status",
                                     success ? "ok" : "failed") &&
              yyjson_mut_obj_add_uint(doc, object, "results", jsonl_result_count);
    if (!ok) {
        yyjson_mut_doc_free(doc);
        errno = ENOMEM;
        jsonl_error();
        return;
    }
    jsonl_write_doc(doc, object);
}

/* Create exclusive matching log files with the requested prefix in CWD. */
static bool open_run_logs(char *log_path, size_t log_path_size,
                          char *jsonl_path, size_t jsonl_path_size,
                          char *started_at, size_t started_at_size,
                          FILE **text_output, FILE **jsonl_output) {
    time_t now = time(NULL);
    struct tm local;
    if (localtime_r(&now, &local) == NULL) return false;
    struct tm utc;
    if (gmtime_r(&now, &utc) == NULL) return false;
    char stamp[32];
    if (strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &local) == 0 ||
        strftime(started_at, started_at_size, "%Y-%m-%dT%H:%M:%SZ", &utc) == 0) {
        errno = EOVERFLOW;
        return false;
    }

    for (unsigned suffix = 0; suffix < 100; ++suffix) {
        char suffix_text[16] = "";
        if (suffix > 0) snprintf(suffix_text, sizeof(suffix_text), "-%u", suffix);
        int log_length = snprintf(log_path, log_path_size,
                                  ".log.te-stsuite-%s%s.log", stamp,
                                  suffix_text);
        int jsonl_length = snprintf(jsonl_path, jsonl_path_size,
                                    ".log.te-stsuite-%s%s.jsonl", stamp,
                                    suffix_text);
        if (log_length < 0 || (size_t)log_length >= log_path_size ||
            jsonl_length < 0 || (size_t)jsonl_length >= jsonl_path_size) {
            errno = ENAMETOOLONG;
            return false;
        }

        int log_fd = open(log_path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
        if (log_fd < 0) {
            if (errno == EEXIST) continue;
            return false;
        }
        int jsonl_fd = open(jsonl_path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC,
                            0644);
        if (jsonl_fd < 0) {
            int saved_errno = errno;
            close(log_fd);
            unlink(log_path);
            if (saved_errno == EEXIST) continue;
            errno = saved_errno;
            return false;
        }

        FILE *text_file = fdopen(log_fd, "w");
        if (text_file == NULL) {
            int saved_errno = errno;
            close(log_fd);
            close(jsonl_fd);
            unlink(log_path);
            unlink(jsonl_path);
            errno = saved_errno;
            return false;
        }
        FILE *jsonl_file_stream = fdopen(jsonl_fd, "w");
        if (jsonl_file_stream == NULL) {
            int saved_errno = errno;
            fclose(text_file);
            close(jsonl_fd);
            unlink(log_path);
            unlink(jsonl_path);
            errno = saved_errno;
            return false;
        }
        *text_output = text_file;
        *jsonl_output = jsonl_file_stream;
        return true;
    }
    errno = EEXIST;
    return false;
}

/* Create a private FIFO and a keepalive writer for one terminal run. */
static int open_control_fifo(char *path, size_t path_size,
                             int *keepalive_fd) {
    *keepalive_fd = -1;
    bool created = false;
    for (unsigned attempt = 0; attempt < 32; ++attempt) {
        int length = snprintf(path, path_size,
                              "/tmp/te-stsuite-%ld-%llu-%u.fifo",
                              (long)getpid(),
                              (unsigned long long)(monotonic_now_ns() %
                                                   1000000000ULL),
                              attempt);
        if (length < 0 || (size_t)length >= path_size) {
            errno = ENAMETOOLONG;
            return -1;
        }
        if (mkfifo(path, 0600) == 0) {
            created = true;
            break;
        }
        if (errno != EEXIST) return -1;
    }
    if (!created) {
        errno = EEXIST;
        return -1;
    }

    int reader = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (reader < 0) {
        int saved_errno = errno;
        unlink(path);
        errno = saved_errno;
        return -1;
    }
    /* Avoid a permanent POLLHUP before the terminal worker opens the FIFO. */
    int keepalive = open(path, O_WRONLY | O_NONBLOCK | O_CLOEXEC);
    if (keepalive < 0) {
        int saved_errno = errno;
        close(reader);
        unlink(path);
        errno = saved_errno;
        return -1;
    }
    *keepalive_fd = keepalive;
    return reader;
}

/* Close the controller FIFO descriptors and remove the private path. */
static void close_control_fifo(int reader_fd, int keepalive_fd,
                               const char *path) {
    if (reader_fd >= 0) close(reader_fd);
    if (keepalive_fd >= 0) close(keepalive_fd);
    if (path != NULL && path[0] != '\0') unlink(path);
}

/* Ask a spawned terminal process group to exit, then reap its direct child. */
static void stop_terminal(pid_t child, bool already_reaped, int child_status) {
    (void)child_status;
    if (!already_reaped) {
        kill(-child, SIGTERM);
        kill(child, SIGTERM);
        for (unsigned attempt = 0; attempt < 100; ++attempt) {
            pid_t result = waitpid(child, &child_status, WNOHANG);
            if (result == child || (result < 0 && errno == ECHILD)) return;
            struct timespec pause = {.tv_sec = 0, .tv_nsec = 20000000};
            nanosleep(&pause, NULL);
        }
        kill(-child, SIGKILL);
        kill(child, SIGKILL);
        while (waitpid(child, &child_status, 0) < 0 && errno == EINTR) {}
    } else {
        kill(-child, SIGTERM);
    }
}

/* Consume a control message and update the interval measurements. */
static bool handle_event(char *line, const TerminalConfig *terminal,
                         Monitor *monitor, SummaryList *summaries,
                         char *active_benchmark, size_t *active_bytes,
                         bool *active, bool *ready, bool *done,
                         bool *worker_failed, bool include_startup,
                         CgroupCheckResult *check_result, WorkerPhase phase,
                         uint64_t launch_ns,
                         StartupSample *startup_sample) {
    if (line[0] == 'R' && line[1] == ' ') {
        long worker_pid = 0;
        long terminal_pid = 0;
        unsigned worker_moved = 0;
        if (sscanf(line + 2, "%ld %ld %u", &worker_pid, &terminal_pid,
                   &worker_moved) != 3) return false;
        monitor_set_worker(monitor, (pid_t)worker_pid, (pid_t)terminal_pid);
        if (phase == WORKER_PHASE_CHECK) {
            const char *detail = NULL;
            MonitorIsolation isolation = monitor_check_isolation(
                monitor, (pid_t)terminal_pid, (pid_t)worker_pid,
                worker_moved != 0, &detail);
            if (check_result != NULL) {
                check_result->isolation = isolation;
                check_result->recorded = true;
                check_result->passed = isolation == MONITOR_ISOLATION_INSIDE;
                check_result->worker_moved = worker_moved != 0;
                snprintf(check_result->detail, sizeof(check_result->detail),
                         "%s", detail == NULL ? monitor_scope_error(monitor)
                                               : detail);
            }
            if (isolation == MONITOR_ISOLATION_INSIDE) {
                report_styled(color_green, "  Cgroup check: passed (%s)\n",
                              detail == NULL ? "ok" : detail);
            } else if (isolation == MONITOR_ISOLATION_OUTSIDE) {
                report_styled(color_red, "  Cgroup check: outside (%s)\n",
                              detail == NULL ? "placement failed" : detail);
            } else {
                report_styled(color_yellow,
                              "  Cgroup check: unavailable (%s)\n",
                              detail == NULL ? monitor_scope_error(monitor) : detail);
            }
        }
        if (phase == WORKER_PHASE_STARTUP) {
            if (startup_sample == NULL) return false;
            uint64_t ready_ns = monotonic_now_ns();
            startup_sample->metrics = monitor_end(monitor, ready_ns);
            startup_sample->duration_ns = ready_ns >= launch_ns
                                              ? ready_ns - launch_ns
                                              : 0;
        } else if (phase == WORKER_PHASE_BENCHMARK && include_startup) {
            MonitorMetrics metrics = monitor_end(monitor, monotonic_now_ns());
            Summary summary = {0};
            snprintf(summary.terminal, sizeof(summary.terminal), "%s",
                     terminal->name);
            snprintf(summary.benchmark, sizeof(summary.benchmark), "startup");
            summary.samples = 1;
            summary.metrics = metrics;
            summary.complete = true;
            if (!summary_push(summaries, &summary)) return false;
            jsonl_write_result(&summaries->items[summaries->count - 1]);
        }
        if (phase != WORKER_PHASE_CHECK) {
            report_styled(color_green, "  Ready\n");
        }
        if (phase == WORKER_PHASE_BENCHMARK && include_startup) {
            print_summary(&summaries->items[summaries->count - 1]);
        }
        *ready = true;
        return true;
    }
    if (line[0] == 'S' && line[1] == ' ') {
        char name[128];
        size_t bytes = 0;
        if (sscanf(line + 2, "%127s %zu", name, &bytes) != 2 || *active) return false;
        snprintf(active_benchmark, 128, "%s", name);
        *active_bytes = bytes;
        *active = true;
        monitor_begin(monitor, monotonic_now_ns());
        report_styled(color_cyan, "  Running: %s\n", name);
        return true;
    }
    if (line[0] == 'E' && line[1] == ' ') {
        char name[128];
        unsigned samples = 0;
        double mean_ms = 0;
        double p90_ms = 0;
        double stddev_ms = 0;
        if (sscanf(line + 2, "%127s %u %lf %lf %lf", name, &samples,
                   &mean_ms, &p90_ms, &stddev_ms) != 5 || !*active ||
            strcmp(name, active_benchmark) != 0) return false;
        Summary summary = {0};
        snprintf(summary.terminal, sizeof(summary.terminal), "%s", terminal->name);
        snprintf(summary.benchmark, sizeof(summary.benchmark), "%s", name);
        summary.bytes = *active_bytes;
        summary.samples = samples;
        summary.sample_mean_ms = mean_ms;
        summary.sample_p90_ms = p90_ms;
        summary.sample_stddev_ms = stddev_ms;
        summary.metrics = monitor_end(monitor, monotonic_now_ns());
        summary.complete = true;
        if (!summary_push(summaries, &summary)) return false;
        jsonl_write_result(&summary);
        print_summary(&summary);
        *active = false;
        active_benchmark[0] = '\0';
        return true;
    }
    if (line[0] == 'F' && line[1] == ' ') {
        report_styled(color_red, "  Worker error: %s\n", line + 2);
        *worker_failed = true;
        *done = true;
        return true;
    }
    if (strcmp(line, "D") == 0) {
        *done = true;
        return true;
    }
    return false;
}

/* Launch one terminal, wait for its worker, and monitor each workload. */
static bool run_terminal(const TerminalConfig *terminal,
                        const AppConfig *config,
                        const RunSelection *selection,
                        const char *executable_path,
                        WorkerPhase phase,
                        SummaryList *summaries,
                        CgroupCheckResult *check_result,
                        bool include_startup,
                        StartupSample *startup_sample) {
    if (startup_sample != NULL) {
        memset(startup_sample, 0, sizeof(*startup_sample));
    }
    char control_path[128] = {0};
    int keepalive_fd = -1;
    int control_fd = open_control_fifo(control_path, sizeof(control_path),
                                       &keepalive_fd);
    if (control_fd < 0) {
        report_styled(color_red, "  Error: cannot create control pipe: %s\n",
                      strerror(errno));
        return false;
    }

    char error[256] = {0};
    char config_path[64] = {0};
    char xresources_path[80] = {0};
    if (!write_inline_config(terminal, config_path, sizeof(config_path), error,
                             sizeof(error))) {
        report_styled(color_red, "  Error: %s\n", error);
        close_control_fifo(control_fd, keepalive_fd, control_path);
        return false;
    }
    if (!write_xresources(terminal, xresources_path,
                          sizeof(xresources_path), error, sizeof(error))) {
        report_styled(color_red, "  Error: %s\n", error);
        close_control_fifo(control_fd, keepalive_fd, control_path);
        cleanup_terminal_files(config_path, xresources_path);
        return false;
    }
    MonitorBackend backend = phase == WORKER_PHASE_CHECK
                                 ? MONITOR_BACKEND_CGROUPV2
                                 : config->monitor_backend;
    Monitor *monitor = monitor_create(backend, config->process_isolation);
    if (monitor == NULL) {
        report_styled(color_red,
                      "  Error: out of memory while creating monitor\n");
        close_control_fifo(control_fd, keepalive_fd, control_path);
        cleanup_terminal_files(config_path, xresources_path);
        return false;
    }
    size_t argc = 0;
    char **argv = expand_command(terminal, config, selection, executable_path,
                                 control_path, config_path, phase,
                                 monitor_worker_cgroup_path(monitor),
                                 &argc, error,
                                 sizeof(error));
    if (argv == NULL) {
        report_styled(color_red, "  Error: %s\n", error);
        close_control_fifo(control_fd, keepalive_fd, control_path);
        cleanup_terminal_files(config_path, xresources_path);
        monitor_destroy(monitor);
        return false;
    }

    int gate[2];
    if (pipe2(gate, O_CLOEXEC) != 0) {
        report_styled(color_red, "  Error: cannot create launch gate: %s\n",
                      strerror(errno));
        free_argv(argv, argc);
        close_control_fifo(control_fd, keepalive_fd, control_path);
        cleanup_terminal_files(config_path, xresources_path);
        monitor_destroy(monitor);
        return false;
    }
    pid_t child = fork();
    if (child < 0) {
        report_styled(color_red, "  Error: cannot fork terminal: %s\n",
                      strerror(errno));
        free_argv(argv, argc);
        close(gate[0]);
        close(gate[1]);
        close_control_fifo(control_fd, keepalive_fd, control_path);
        cleanup_terminal_files(config_path, xresources_path);
        monitor_destroy(monitor);
        return false;
    }
    if (child == 0) {
        close(gate[1]);
        setpgid(0, 0);
        char release = 0;
        ssize_t gate_result;
        do {
            gate_result = read(gate[0], &release, 1);
        } while (gate_result < 0 && errno == EINTR);
        close(gate[0]);
        if (gate_result != 1 || release != 'G') _exit(126);
        /* Keep the runner's delegated-cgroup hint private from launched TEs. */
        unsetenv("TE_STSUITE_DELEGATED_SUBGROUP");
        if (xresources_path[0] != '\0' &&
            setenv("XENVIRONMENT", xresources_path, 1) != 0) {
            dprintf(STDERR_FILENO,
                    "te-stsuite: cannot set XENVIRONMENT: %s\n",
                    strerror(errno));
            _exit(126);
        }
        execvp(argv[0], argv);
        dprintf(STDERR_FILENO, "te-stsuite: cannot start '%s': %s\n",
                argv[0], strerror(errno));
        _exit(127);
    }
    close(gate[0]);
    setpgid(child, child);
    free_argv(argv, argc);
    if (!monitor_attach_terminal(monitor, child)) {
        report_styled(color_red, "  Error: cannot isolate terminal process: %s\n",
                      monitor_scope_error(monitor));
        close(gate[1]);
        stop_terminal(child, false, 0);
        close_control_fifo(control_fd, keepalive_fd, control_path);
        cleanup_terminal_files(config_path, xresources_path);
        monitor_destroy(monitor);
        return false;
    }
    uint64_t launch_ns = monotonic_now_ns();
    monitor_begin(monitor, launch_ns);
    char release = 'G';
    ssize_t released;
    do {
        released = write(gate[1], &release, 1);
    } while (released < 0 && errno == EINTR);
    close(gate[1]);
    if (released != 1) {
        report_styled(color_red, "  Error: cannot release terminal launch gate\n");
        stop_terminal(child, false, 0);
        close_control_fifo(control_fd, keepalive_fd, control_path);
        cleanup_terminal_files(config_path, xresources_path);
        monitor_destroy(monitor);
        return false;
    }
    if (phase == WORKER_PHASE_CHECK) {
        report_styled(color_cyan, "  Checking process placement...\n");
    } else if (phase == WORKER_PHASE_WARMUP) {
        report_styled(color_cyan,
                      config->process_isolation == PROCESS_ISOLATION_CGROUPV2
                          ? "  Warming terminal on isolated CPU %s...\n"
                          : "  Warming terminal pinned to CPU %s...\n",
                      monitor_selected_cpu(monitor));
    } else {
        report_styled(color_cyan,
                      config->process_isolation == PROCESS_ISOLATION_CGROUPV2
                          ? "  Starting on isolated CPU %s...\n"
                          : "  Starting pinned to CPU %s...\n",
                      monitor_selected_cpu(monitor));
    }

    uint64_t deadline = launch_ns +
                        (uint64_t)config->options.startup_timeout_seconds *
                            1000000000ULL;
    bool worker_started = false;
    bool child_reaped = false;
    int child_status = 0;
    while (!worker_started && monotonic_now_ns() < deadline) {
        struct pollfd event = {.fd = control_fd, .events = POLLIN};
        int polled = poll(&event, 1, 20);
        monitor_tick(monitor, monotonic_now_ns());
        if (polled < 0 && errno != EINTR) break;
        if (event.revents & POLLIN) {
            worker_started = true;
            if (keepalive_fd >= 0) {
                /* Worker data now proves a real writer exists; preserve EOF. */
                close(keepalive_fd);
                keepalive_fd = -1;
            }
        }
        if (!child_reaped) {
            pid_t status = waitpid(child, &child_status, WNOHANG);
            if (status == child) child_reaped = true;
        }
        if (child_reaped && !worker_started) break;
    }
    if (worker_started) unlink(control_path);
    if (!worker_started) {
        monitor_end(monitor, monotonic_now_ns());
        if (child_reaped) {
            report_styled(color_red,
                          "  Error: terminal exited before the worker sent a control event\n");
        } else {
            report_styled(color_red, "  Error: startup timed out after %u seconds\n",
                          config->options.startup_timeout_seconds);
        }
        stop_terminal(child, child_reaped, child_status);
        close_control_fifo(control_fd, keepalive_fd, control_path);
        monitor_destroy(monitor);
        cleanup_terminal_files(config_path, xresources_path);
        return false;
    }

    bool ready = false;
    bool done = false;
    bool worker_failed = false;
    bool active = false;
    char active_benchmark[128] = {0};
    size_t active_bytes = 0;
    char input[2048];
    size_t input_length = 0;
    uint64_t seconds_per_benchmark = config->options.max_seconds;
    if (phase == WORKER_PHASE_WARMUP && config->options.warmup > 1) {
        uint64_t warmup_count = config->options.warmup;
        seconds_per_benchmark =
            seconds_per_benchmark > UINT64_MAX / warmup_count
                ? UINT64_MAX
                : seconds_per_benchmark * warmup_count;
    }
    uint64_t seconds_limit = UINT64_MAX / 1000000000ULL;
    uint64_t worker_allowance;
    if (seconds_per_benchmark >
        (seconds_limit - 10) / te_stsuite_benchmark_count) {
        worker_allowance = UINT64_MAX;
    } else {
        worker_allowance = seconds_per_benchmark *
                           te_stsuite_benchmark_count * 1000000000ULL +
                           10000000000ULL;
    }
    uint64_t worker_deadline = deadline > UINT64_MAX - worker_allowance
                                   ? UINT64_MAX
                                   : deadline + worker_allowance;

    while (!done && monotonic_now_ns() < worker_deadline) {
        struct pollfd event = {.fd = control_fd, .events = POLLIN};
        int polled = poll(&event, 1, 20);
        uint64_t now = monotonic_now_ns();
        if (!ready || active) monitor_tick(monitor, now);
        if (polled < 0 && errno != EINTR) break;
        if (event.revents & (POLLIN | POLLHUP)) {
            char chunk[512];
            ssize_t bytes = read(control_fd, chunk, sizeof(chunk));
            if (bytes == 0) break;
            if (bytes < 0 && errno != EAGAIN && errno != EINTR) break;
            if (bytes > 0) {
                if (input_length + (size_t)bytes >= sizeof(input)) {
                    report_styled(color_red,
                                  "  Error: oversized worker control message\n");
                    break;
                }
                memcpy(input + input_length, chunk, (size_t)bytes);
                input_length += (size_t)bytes;
                size_t consumed = 0;
                for (size_t i = 0; i < input_length; ++i) {
                    if (input[i] != '\n') continue;
                    input[i] = '\0';
                    bool handled = handle_event(input + consumed, terminal,
                                                monitor, summaries,
                                                active_benchmark, &active_bytes,
                                                &active, &ready, &done,
                                                &worker_failed,
                                                include_startup,
                                                check_result, phase,
                                                launch_ns, startup_sample);
                    consumed = i + 1;
                    if (!handled && !done) {
                        report_styled(color_red,
                                      "  Error: invalid worker control event\n");
                        done = true;
                        break;
                    }
                    if (done) break;
                }
                if (consumed > 0) {
                    memmove(input, input + consumed, input_length - consumed);
                    input_length -= consumed;
                }
            }
        }
        if (!ready && now >= deadline) {
            report_styled(color_red,
                          "  Error: worker did not become ready before the startup timeout\n");
            break;
        }
    }

    if (active) {
        MonitorMetrics partial = monitor_end(monitor, monotonic_now_ns());
        Summary summary = {0};
        snprintf(summary.terminal, sizeof(summary.terminal), "%s", terminal->name);
        snprintf(summary.benchmark, sizeof(summary.benchmark), "%s",
                 active_benchmark);
        summary.bytes = active_bytes;
        summary.metrics = partial;
        summary.complete = false;
        if (summary_push(summaries, &summary)) {
            jsonl_write_result(&summary);
            print_summary(&summary);
        }
    }
    close_control_fifo(control_fd, keepalive_fd, control_path);
    stop_terminal(child, child_reaped, child_status);
    monitor_destroy(monitor);
    cleanup_terminal_files(config_path, xresources_path);
    return ready && done && !worker_failed &&
           (phase != WORKER_PHASE_CHECK ||
            (check_result != NULL && check_result->passed));
}

/* Order independent startup durations for percentile calculations. */
static int compare_startup_duration(const void *left, const void *right) {
    uint64_t a = *(const uint64_t *)left;
    uint64_t b = *(const uint64_t *)right;
    return (a > b) - (a < b);
}

/* Launch until startup has enough samples and enough measured wall time. */
static bool run_startup_benchmark(const TerminalConfig *terminal,
                                 const AppConfig *config,
                                 const RunSelection *selection,
                                 const char *executable_path,
                                 SummaryList *summaries) {
    uint64_t *durations = NULL;
    size_t capacity = 0;
    unsigned samples = 0;
    uint64_t total_duration_ns = 0;
    double total_cpu_seconds = 0;
    long double memory_integral = 0;
    uint64_t peak_memory_bytes = 0;
    bool resources_available = true;
    bool complete = false;
    bool ok = true;
    uint64_t target_duration_ns =
        (uint64_t)config->options.startup_min_seconds * 1000000000ULL;

    while (!complete) {
        if (samples == capacity) {
            size_t next_capacity = capacity == 0 ? 16 : capacity * 2;
            if (next_capacity < capacity ||
                next_capacity > SIZE_MAX / sizeof(*durations)) {
                report_styled(color_red,
                              "  Error: too many startup samples to store\n");
                ok = false;
                break;
            }
            uint64_t *next = realloc(durations,
                                     next_capacity * sizeof(*durations));
            if (next == NULL) {
                report_styled(color_red,
                              "  Error: out of memory collecting startup samples\n");
                ok = false;
                break;
            }
            durations = next;
            capacity = next_capacity;
        }

        report_styled(color_dim, "  Startup sample %u\n", samples + 1);
        StartupSample sample;
        if (!run_terminal(terminal, config, selection, executable_path,
                          WORKER_PHASE_STARTUP, summaries, NULL, false,
                          &sample)) {
            ok = false;
            break;
        }

        durations[samples++] = sample.duration_ns;
        total_duration_ns = sample.duration_ns > UINT64_MAX - total_duration_ns
                                ? UINT64_MAX
                                : total_duration_ns + sample.duration_ns;
        resources_available = resources_available &&
                              sample.metrics.resources_available;
        total_cpu_seconds += sample.metrics.cpu_seconds;
        memory_integral += (long double)sample.metrics.average_memory_bytes *
                           sample.metrics.wall_seconds;
        if (sample.metrics.peak_memory_bytes > peak_memory_bytes) {
            peak_memory_bytes = sample.metrics.peak_memory_bytes;
        }

        bool enough_samples = samples >= config->options.startup_min_samples;
        bool enough_time = total_duration_ns >= target_duration_ns;
        bool sample_limit = config->options.max_samples > 0 &&
                            samples >= config->options.max_samples;
        complete = (enough_samples && enough_time) || sample_limit;
    }

    if (samples > 0) {
        qsort(durations, samples, sizeof(*durations),
              compare_startup_duration);
        long double sum = 0;
        for (unsigned i = 0; i < samples; ++i) sum += durations[i];
        long double mean = sum / samples;
        long double variance = 0;
        if (samples > 1) {
            for (unsigned i = 0; i < samples; ++i) {
                long double delta = (long double)durations[i] - mean;
                variance += delta * delta;
            }
            variance /= samples - 1;
        }
        size_t percentile_index = ((size_t)samples * 90 + 99) / 100 - 1;
        Summary summary = {0};
        snprintf(summary.terminal, sizeof(summary.terminal), "%s",
                 terminal->name);
        snprintf(summary.benchmark, sizeof(summary.benchmark), "startup");
        summary.samples = samples;
        summary.sample_mean_ms = (double)(mean / 1000000.0L);
        summary.sample_p90_ms = (double)durations[percentile_index] / 1000000.0;
        summary.sample_stddev_ms = (double)(sqrtl(variance) / 1000000.0L);
        summary.metrics.resources_available = resources_available;
        summary.metrics.wall_seconds = (double)(mean / 1000000000.0L);
        summary.metrics.cpu_seconds = total_cpu_seconds / samples;
        summary.metrics.average_memory_bytes = total_duration_ns > 0
                                                   ? (double)(memory_integral /
                                                              ((long double)total_duration_ns /
                                                               1000000000.0L))
                                                   : 0;
        summary.metrics.peak_memory_bytes = peak_memory_bytes;
        summary.complete = complete;
        if (!summary_push(summaries, &summary)) {
            report_styled(color_red,
                          "  Error: cannot store the startup summary\n");
            ok = false;
        } else {
            jsonl_write_result(&summaries->items[summaries->count - 1]);
            print_summary(&summaries->items[summaries->count - 1]);
        }
    }

    free(durations);
    return ok && complete;
}

/* Atomically point a stable name at one completed run log. */
static bool replace_latest_log_link(const char *link_path,
                                    const char *target_path) {
    char temporary_path[128];
    for (unsigned attempt = 0; attempt < 100; ++attempt) {
        int length = snprintf(temporary_path, sizeof(temporary_path),
                              ".%s.tmp-%ld-%u", link_path, (long)getpid(),
                              attempt);
        if (length < 0 || (size_t)length >= sizeof(temporary_path)) {
            errno = ENAMETOOLONG;
            return false;
        }
        if (symlink(target_path, temporary_path) != 0) {
            if (errno == EEXIST) continue;
            return false;
        }
        if (rename(temporary_path, link_path) != 0) {
            int saved_errno = errno;
            unlink(temporary_path);
            errno = saved_errno;
            return false;
        }
        return true;
    }
    errno = EEXIST;
    return false;
}

/* Close run outputs before publishing their latest-log links. */
static int finish_run_logs(bool succeeded, const char *log_path,
                           const char *jsonl_path) {
    if (fclose(jsonl_file) != 0) succeeded = false;
    jsonl_file = NULL;
    if (fclose(log_file) != 0) succeeded = false;
    log_file = NULL;

    bool log_linked = replace_latest_log_link("log-latest.log", log_path);
    int log_errno = errno;
    bool jsonl_linked =
        replace_latest_log_link("log-latest.jsonl", jsonl_path);
    int jsonl_errno = errno;
    if (!log_linked) {
        fprintf(stderr, "te-stsuite: cannot update log-latest.log: %s\n",
                strerror(log_errno));
        succeeded = false;
    }
    if (!jsonl_linked) {
        fprintf(stderr, "te-stsuite: cannot update log-latest.jsonl: %s\n",
                strerror(jsonl_errno));
        succeeded = false;
    }
    return succeeded ? 0 : 1;
}

/* Run configured terminals sequentially and write both per-terminal and final views. */
int controller_run(const AppConfig *config, const RunSelection *selection,
                   const char *executable_path) {
    char log_path[128];
    char jsonl_path[128];
    char started_at[32];
    const char *term = getenv("TERM");
    color_enabled = isatty(STDOUT_FILENO) && getenv("NO_COLOR") == NULL &&
                    (term == NULL || strcmp(term, "dumb") != 0);
    jsonl_result_count = 0;
    jsonl_failed = false;
    if (!open_run_logs(log_path, sizeof(log_path), jsonl_path,
                       sizeof(jsonl_path), started_at, sizeof(started_at),
                       &log_file, &jsonl_file)) {
        fprintf(stderr, "te-stsuite: cannot create run logs: %s\n",
                strerror(errno));
        return 1;
    }

    SummaryList summaries = {0};
    jsonl_write_run(config, started_at);
    report_styled(color_bold_cyan, "te-stsuite\n");
    report_styled(color_dim, "Terminal emulator benchmark suite\n");
    report_styled(color_dim, "Configuration  %s\n", config->path);
    report_styled(color_dim, "Monitor        %s\n",
                  monitor_backend_name(config->monitor_backend));
    report_styled(color_dim, "Isolation      %s\n",
                  process_isolation_name(config->process_isolation));
    report_styled(color_dim, "Text log       %s\n", log_path);
    report_styled(color_dim, "JSONL log      %s\n\n", jsonl_path);
    size_t selected_terminal_count = 0;
    for (size_t i = 0; i < config->terminal_count; ++i) {
        if (terminal_is_selected(&config->terminals[i], selection)) {
            selected_terminal_count++;
        }
    }
    CgroupCheckResult *check_results =
        calloc(selected_terminal_count == 0 ? 1 : selected_terminal_count,
               sizeof(*check_results));
    if (check_results == NULL) {
        report_styled(color_red,
                      "te-stsuite: cannot allocate cgroup check results\n");
        jsonl_write_end(false);
        report_styled(color_dim, "\nLogs written\n  Text   %s\n  JSONL  %s\n",
                      log_path, jsonl_path);
        return finish_run_logs(false, log_path, jsonl_path);
    }

    report_styled(color_bold_cyan, "Process isolation checks\n");
    size_t terminal_number = 0;
    size_t check_index = 0;
    bool checks_passed = true;
    for (size_t i = 0; i < config->terminal_count; ++i) {
        if (!terminal_is_selected(&config->terminals[i], selection)) continue;
        terminal_number++;
        CgroupCheckResult *result = &check_results[check_index++];
        result->terminal = config->terminals[i].name;
        result->isolation = MONITOR_ISOLATION_UNAVAILABLE;
        snprintf(result->detail, sizeof(result->detail),
                 "check did not complete; see the terminal error above");
        report_styled(color_cyan, "[%zu/%zu] %s\n", terminal_number,
                      selected_terminal_count, config->terminals[i].name);
        bool ok = run_terminal(&config->terminals[i], config, selection,
                               executable_path, WORKER_PHASE_CHECK,
                               &summaries, result, false, NULL);
        if (!result->recorded) {
            snprintf(result->detail, sizeof(result->detail),
                     "check did not complete; see the terminal error above");
        } else if (!ok && result->passed) {
            result->passed = false;
            result->isolation = MONITOR_ISOLATION_UNAVAILABLE;
            snprintf(result->detail, sizeof(result->detail),
                     "terminal or worker exited before the check completed");
        }
        jsonl_write_cgroup_check(result->terminal, result->isolation,
                                 result->detail, result->worker_moved);
        report_styled(ok ? color_green : color_red,
                      "  %s: Cgroup check\n\n",
                      ok ? "Finished" : "Failed");
        if (!result->passed) checks_passed = false;
    }

    if (!checks_passed) {
        report_styled(color_red,
                      "\nBenchmark run stopped before warmup because cgroup checks failed.\n");
        print_cgroup_check_summary(check_results, check_index);
        jsonl_write_end(false);
        report_styled(color_dim, "\nLogs written\n  Text   %s\n  JSONL  %s\n",
                      log_path, jsonl_path);
        free(check_results);
        free(summaries.items);
        return finish_run_logs(false, log_path, jsonl_path);
    }

    bool all_ok = true;
    size_t benchmark_count = selected_benchmark_count(selection);
    bool startup_selected = selection_includes_benchmark(selection, "startup");
    size_t workload_count = benchmark_count - (startup_selected ? 1 : 0);
    if (benchmark_count == 0) {
        report_styled(color_bold_cyan,
                      "No benchmarks selected; skipping warmup and measurement.\n");
    } else {
        report_styled(color_bold_cyan, "Warmup and benchmarks\n");
    }
    terminal_number = 0;
    for (size_t i = 0; i < config->terminal_count; ++i) {
        if (!terminal_is_selected(&config->terminals[i], selection)) continue;
        terminal_number++;
        report_styled(color_cyan, "[%zu/%zu] %s\n",
                      terminal_number, selected_terminal_count,
                      config->terminals[i].name);
        if (benchmark_count > 0) {
            report_styled(color_dim, "  Warmup\n");
            bool warmup_ok = run_terminal(&config->terminals[i], config,
                                          selection, executable_path,
                                          WORKER_PHASE_WARMUP, &summaries,
                                          NULL, false, NULL);
            if (!warmup_ok) all_ok = false;
            report_styled(warmup_ok ? color_green : color_red,
                          "  %s: Warmup\n", warmup_ok ? "Finished" : "Failed");

            if (startup_selected) {
                report_styled(color_dim, "  Startup benchmark\n");
                bool startup_ok = run_startup_benchmark(
                    &config->terminals[i], config, selection,
                    executable_path, &summaries);
                if (!startup_ok) all_ok = false;
                report_styled(startup_ok ? color_green : color_red,
                              "  %s: Startup benchmark\n",
                              startup_ok ? "Finished" : "Failed");
            }

            if (workload_count > 0) {
                report_styled(color_dim, "  Benchmarks\n");
                size_t before = summaries.count;
                bool benchmark_ok = run_terminal(
                    &config->terminals[i], config, selection, executable_path,
                    WORKER_PHASE_BENCHMARK, &summaries, NULL, false, NULL);
                if (!benchmark_ok) all_ok = false;
                size_t collected = summaries.count - before;
                report_styled(benchmark_ok ? color_green : color_red,
                              "  %s: %zu benchmark result%s\n",
                              benchmark_ok ? "Finished" : "Failed", collected,
                              collected == 1 ? "" : "s");
            }
        }
        report_styled(NULL, "\n");
    }

    print_collective_summary(&summaries);
    if (jsonl_failed) all_ok = false;
    jsonl_write_end(all_ok);
    if (jsonl_failed) all_ok = false;
    report_styled(color_dim, "\nLogs written\n  Text   %s\n  JSONL  %s\n",
                  log_path, jsonl_path);

    free(check_results);
    free(summaries.items);
    return finish_run_logs(all_ok, log_path, jsonl_path);
}
