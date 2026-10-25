#define _GNU_SOURCE
#include "cvtebench.h"
#include "yyjson.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <sched.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
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
    ResourceMetrics metrics;
    bool complete;
} Summary;

typedef struct {
    Summary *items;
    size_t count;
    size_t capacity;
} SummaryList;

static FILE *log_file;
static FILE *jsonl_file;
static size_t jsonl_result_count;
static bool jsonl_failed;

/* Match a terminal name directly, or its base name across config variants. */
static bool terminal_name_matches(const char *configured, const char *requested) {
    size_t length = strlen(requested);
    return strcmp(configured, requested) == 0 ||
           (strncmp(configured, requested, length) == 0 &&
            configured[length] == '/');
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
        for (size_t j = 0; j < cvtebench_benchmark_count; ++j) {
            if (strcmp(cvtebench_benchmarks[j].name,
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

/* Write one formatted message to both the console and the run log. */
static void report(const char *format, ...) {
    va_list args;
    va_start(args, format);
    va_list copy;
    va_copy(copy, args);
    vfprintf(stdout, format, args);
    if (log_file != NULL) vfprintf(log_file, format, copy);
    va_end(copy);
    va_end(args);
    fflush(stdout);
    if (log_file != NULL) fflush(log_file);
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
                             const char *socket_path, const char *config_path,
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
    size_t argc = words.count + 4 + selected_count * 2;
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
            argv[output++] = strdup("--worker-socket");
            argv[output++] = strdup(socket_path);
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

/* Create a private on-disk config because terminal CLI options need a path. */
static bool write_inline_config(const TerminalConfig *terminal, char *path,
                                size_t path_size, char *error,
                                size_t error_size) {
    if (terminal->inline_config == NULL) {
        path[0] = '\0';
        return true;
    }
    if (snprintf(path, path_size, "/tmp/cvtebench-config-XXXXXX") >=
        (int)path_size) {
        snprintf(error, error_size, "temporary config path buffer is too small");
        return false;
    }
    int fd = mkstemp(path);
    if (fd < 0) {
        snprintf(error, error_size, "cannot create temporary config: %s",
                 strerror(errno));
        return false;
    }
    size_t written = 0;
    while (written < terminal->inline_config_length) {
        ssize_t count = write(fd, terminal->inline_config + written,
                              terminal->inline_config_length - written);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) {
            snprintf(error, error_size, "cannot write temporary config: %s",
                     count < 0 ? strerror(errno) : "short write");
            close(fd);
            unlink(path);
            return false;
        }
        written += (size_t)count;
    }
    if (close(fd) != 0) {
        snprintf(error, error_size, "cannot close temporary config: %s",
                 strerror(errno));
        unlink(path);
        return false;
    }
    return true;
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

/* Print measurements with core-normalized CPU and tree-resident memory. */
static void print_summary(const Summary *summary) {
    double cpu_percent = summary->metrics.wall_seconds > 0
                             ? summary->metrics.cpu_seconds /
                                   summary->metrics.wall_seconds * 100.0
                             : 0;
    report("  %-28s %8.2f ms | CPU %7.2f%% (%8.2f ms) | RAM avg %7.2f MiB, peak %7.2f MiB",
           summary->benchmark, summary->metrics.wall_seconds * 1000.0,
           cpu_percent, summary->metrics.cpu_seconds * 1000.0,
           summary->metrics.average_rss_bytes / 1048576.0,
           (double)summary->metrics.peak_rss_bytes / 1048576.0);
    if (summary->samples > 0) {
        report(" | write avg %.2f ms (p90 %.2f ms, sd %.2f ms) | samples %u",
               summary->sample_mean_ms, summary->sample_p90_ms,
               summary->sample_stddev_ms, summary->samples);
    }
    if (summary->bytes > 0) {
        report(" | %zu bytes/sample", summary->bytes);
    }
    report("\n");
}

/* Report the first JSONL failure while allowing the run log to continue. */
static bool jsonl_error(void) {
    if (!jsonl_failed) {
        int saved_errno = errno;
        fprintf(stderr, "cvtebench: JSONL logging failed: %s\n",
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
              yyjson_mut_obj_add_uint(doc, object, "schema", 1) &&
              yyjson_mut_obj_add_str(doc, object, "started_at", started_at) &&
              yyjson_mut_obj_add_str(doc, object, "config", config->path);
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
              yyjson_mut_obj_add_real(doc, object, "cpu_s",
                                      summary->metrics.cpu_seconds) &&
              yyjson_mut_obj_add_real(doc, object, "cpu_pct", cpu_percent) &&
              yyjson_mut_obj_add_uint(doc, object, "rss_avg_bytes",
                                      (uint64_t)summary->metrics.average_rss_bytes) &&
              yyjson_mut_obj_add_uint(doc, object, "rss_peak_bytes",
                                      summary->metrics.peak_rss_bytes) &&
              yyjson_mut_obj_add_uint(doc, object, "samples", summary->samples) &&
              yyjson_mut_obj_add_uint(doc, object, "payload_bytes",
                                      (uint64_t)summary->bytes) &&
              yyjson_mut_obj_add_bool(doc, object, "complete", summary->complete);
    if (ok && summary->bytes > 0) {
        ok = yyjson_mut_obj_add_real(doc, object, "write_avg_ms",
                                     summary->sample_mean_ms) &&
             yyjson_mut_obj_add_real(doc, object, "write_p90_ms",
                                     summary->sample_p90_ms) &&
             yyjson_mut_obj_add_real(doc, object, "write_sd_ms",
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

/* Create exclusive, matching datetime-named text and JSONL files in CWD. */
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
                                  "cvtebench-%s%s.log", stamp, suffix_text);
        int jsonl_length = snprintf(jsonl_path, jsonl_path_size,
                                    "cvtebench-%s%s.jsonl", stamp, suffix_text);
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

/* Create the private local control socket used by one terminal run. */
static int open_control_socket(char *path, size_t path_size) {
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0) return -1;
    snprintf(path, path_size, "/tmp/cvtebench-%ld-%llu.sock", (long)getpid(),
             (unsigned long long)(monotonic_now_ns() % 1000000000ULL));
    struct sockaddr_un address = {.sun_family = AF_UNIX};
    if (strlen(path) >= sizeof(address.sun_path)) {
        close(fd);
        errno = ENAMETOOLONG;
        return -1;
    }
    strcpy(address.sun_path, path);
    unlink(path);
    if (bind(fd, (struct sockaddr *)&address, sizeof(address)) != 0 ||
        chmod(path, 0600) != 0 || listen(fd, 1) != 0) {
        int saved_errno = errno;
        close(fd);
        unlink(path);
        errno = saved_errno;
        return -1;
    }
    return fd;
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
                         ProcessMonitor *monitor, SummaryList *summaries,
                         char *active_benchmark, size_t *active_bytes,
                         bool *active, bool *ready, bool *done,
                         bool *worker_failed, bool include_startup,
                         uint64_t launch_ns) {
    if (line[0] == 'R' && line[1] == ' ') {
        long worker_pid = 0;
        long terminal_pid = 0;
        if (sscanf(line + 2, "%ld %ld", &worker_pid, &terminal_pid) != 2) return false;
        monitor_set_worker(monitor, (pid_t)worker_pid, (pid_t)terminal_pid);
        ResourceMetrics metrics = monitor_end(monitor, monotonic_now_ns());
        if (include_startup) {
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
        report("[%s] ready\n", terminal->name);
        if (include_startup) print_summary(&summaries->items[summaries->count - 1]);
        (void)launch_ns;
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
        report("[%s] running %s\n", terminal->name, name);
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
        report("[%s] worker error: %s\n", terminal->name, line + 2);
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
                        SummaryList *summaries) {
    char socket_path[sizeof(((struct sockaddr_un *)0)->sun_path)];
    int listener = open_control_socket(socket_path, sizeof(socket_path));
    if (listener < 0) {
        report("[%s] cannot create control socket: %s\n", terminal->name,
               strerror(errno));
        return false;
    }

    char error[256] = {0};
    char config_path[64] = {0};
    if (!write_inline_config(terminal, config_path, sizeof(config_path), error,
                             sizeof(error))) {
        report("[%s] %s\n", terminal->name, error);
        close(listener);
        unlink(socket_path);
        return false;
    }
    size_t argc = 0;
    char **argv = expand_command(terminal, config, selection, executable_path,
                                 socket_path, config_path, &argc, error,
                                 sizeof(error));
    if (argv == NULL) {
        report("[%s] %s\n", terminal->name, error);
        close(listener);
        unlink(socket_path);
        if (config_path[0] != '\0') unlink(config_path);
        return false;
    }

    uint64_t launch_ns = monotonic_now_ns();
    pid_t child = fork();
    if (child < 0) {
        report("[%s] cannot fork terminal: %s\n", terminal->name,
               strerror(errno));
        free_argv(argv, argc);
        close(listener);
        unlink(socket_path);
        if (config_path[0] != '\0') unlink(config_path);
        return false;
    }
    if (child == 0) {
        setpgid(0, 0);
        cpu_set_t cpus;
        CPU_ZERO(&cpus);
        CPU_SET(0, &cpus);
        if (sched_setaffinity(0, sizeof(cpus), &cpus) != 0) {
            dprintf(STDERR_FILENO,
                    "cvtebench: cannot pin terminal to CPU 0: %s\n",
                    strerror(errno));
            _exit(126);
        }
        execvp(argv[0], argv);
        dprintf(STDERR_FILENO, "cvtebench: cannot start '%s': %s\n",
                argv[0], strerror(errno));
        _exit(127);
    }
    setpgid(child, child);
    free_argv(argv, argc);

    ProcessMonitor *monitor = monitor_create(child, 0);
    if (monitor == NULL) {
        report("[%s] out of memory while starting process monitor\n",
               terminal->name);
        stop_terminal(child, false, 0);
        close(listener);
        unlink(socket_path);
        if (config_path[0] != '\0') unlink(config_path);
        return false;
    }
    monitor_begin(monitor, launch_ns);
    report("[%s] started with CPU affinity 0\n", terminal->name);

    uint64_t deadline = launch_ns +
                        (uint64_t)config->options.startup_timeout_seconds *
                            1000000000ULL;
    int connection = -1;
    bool child_reaped = false;
    int child_status = 0;
    while (connection < 0 && monotonic_now_ns() < deadline) {
        struct pollfd event = {.fd = listener, .events = POLLIN};
        poll(&event, 1, 20);
        monitor_tick(monitor, monotonic_now_ns());
        if (event.revents & POLLIN) {
            connection = accept4(listener, NULL, NULL, SOCK_CLOEXEC | SOCK_NONBLOCK);
        }
        if (!child_reaped) {
            pid_t status = waitpid(child, &child_status, WNOHANG);
            if (status == child) child_reaped = true;
        }
        if (child_reaped && connection < 0) break;
    }
    close(listener);
    unlink(socket_path);
    if (connection < 0) {
        monitor_end(monitor, monotonic_now_ns());
        if (child_reaped) {
            report("[%s] terminal exited before the benchmark worker connected\n",
                   terminal->name);
        } else {
            report("[%s] startup timed out after %u seconds\n", terminal->name,
                   config->options.startup_timeout_seconds);
        }
        stop_terminal(child, child_reaped, child_status);
        monitor_destroy(monitor);
        if (config_path[0] != '\0') unlink(config_path);
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
    uint64_t seconds_limit = UINT64_MAX / 1000000000ULL;
    uint64_t worker_allowance;
    if (seconds_per_benchmark >
        (seconds_limit - 10) / cvtebench_benchmark_count) {
        worker_allowance = UINT64_MAX;
    } else {
        worker_allowance = seconds_per_benchmark *
                           cvtebench_benchmark_count * 1000000000ULL +
                           10000000000ULL;
    }
    uint64_t worker_deadline = deadline > UINT64_MAX - worker_allowance
                                   ? UINT64_MAX
                                   : deadline + worker_allowance;

    while (!done && monotonic_now_ns() < worker_deadline) {
        struct pollfd event = {.fd = connection, .events = POLLIN};
        int polled = poll(&event, 1, 20);
        uint64_t now = monotonic_now_ns();
        if (!ready || active) monitor_tick(monitor, now);
        if (polled < 0 && errno != EINTR) break;
        if (event.revents & (POLLIN | POLLHUP)) {
            char chunk[512];
            ssize_t bytes = read(connection, chunk, sizeof(chunk));
            if (bytes == 0) break;
            if (bytes < 0 && errno != EAGAIN && errno != EINTR) break;
            if (bytes > 0) {
                if (input_length + (size_t)bytes >= sizeof(input)) {
                    report("[%s] oversized worker control message\n",
                           terminal->name);
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
                                                selection_includes_benchmark(
                                                    selection, "startup"),
                                                launch_ns);
                    consumed = i + 1;
                    if (!handled && !done) {
                        report("[%s] invalid worker control event\n",
                               terminal->name);
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
            report("[%s] worker did not become ready before the startup timeout\n",
                   terminal->name);
            break;
        }
    }

    if (active) {
        ResourceMetrics partial = monitor_end(monitor, monotonic_now_ns());
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
    close(connection);
    stop_terminal(child, child_reaped, child_status);
    monitor_destroy(monitor);
    if (config_path[0] != '\0') unlink(config_path);
    return ready && done && !worker_failed;
}

/* Run configured terminals sequentially and write both per-terminal and final views. */
int controller_run(const AppConfig *config, const RunSelection *selection,
                   const char *executable_path) {
    char log_path[128];
    char jsonl_path[128];
    char started_at[32];
    jsonl_result_count = 0;
    jsonl_failed = false;
    if (!open_run_logs(log_path, sizeof(log_path), jsonl_path,
                       sizeof(jsonl_path), started_at, sizeof(started_at),
                       &log_file, &jsonl_file)) {
        fprintf(stderr, "cvtebench: cannot create run logs: %s\n",
                strerror(errno));
        return 1;
    }

    SummaryList summaries = {0};
    jsonl_write_run(config, started_at);
    report("cvtebench run started: %s\n", config->path);
    report("Log: %s\nJSONL: %s\n\n", log_path, jsonl_path);
    bool all_ok = true;
    for (size_t i = 0; i < config->terminal_count; ++i) {
        bool selected = selection == NULL || selection->terminal_count == 0;
        for (size_t j = 0; !selected && j < selection->terminal_count; ++j) {
            selected = terminal_name_matches(config->terminals[i].name,
                                             selection->terminal_names[j]);
        }
        if (!selected) continue;
        report("Terminal: %s\n", config->terminals[i].name);
        size_t before = summaries.count;
        bool ok = run_terminal(&config->terminals[i], config, selection,
                               executable_path, &summaries);
        if (!ok) all_ok = false;
        report("Summary for %s\n", config->terminals[i].name);
        for (size_t row = before; row < summaries.count; ++row) {
            print_summary(&summaries.items[row]);
        }
        report("Finished %s: %zu summaries collected\n\n",
               config->terminals[i].name, summaries.count - before);
    }

    report("Collective summary\n");
    for (size_t i = 0; i < summaries.count; ++i) {
        report("[%s]", summaries.items[i].terminal);
        print_summary(&summaries.items[i]);
    }
    if (jsonl_failed) all_ok = false;
    jsonl_write_end(all_ok);
    if (jsonl_failed) all_ok = false;
    report("\nLog saved to %s\nJSONL saved to %s\n", log_path, jsonl_path);

    free(summaries.items);
    if (fclose(jsonl_file) != 0) all_ok = false;
    jsonl_file = NULL;
    if (fclose(log_file) != 0) all_ok = false;
    log_file = NULL;
    return all_ok ? 0 : 1;
}
