#define _GNU_SOURCE
#include "te-stsuite.h"

#include <errno.h>
#include <math.h>
#include <stdarg.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

/* Send a complete control event even when the socket accepts partial writes. */
static bool send_event(int socket_fd, const char *format, ...) {
    char message[512];
    va_list args;
    va_start(args, format);
    int length = vsnprintf(message, sizeof(message), format, args);
    va_end(args);
    if (length < 0 || (size_t)length >= sizeof(message)) return false;

    size_t offset = 0;
    while (offset < (size_t)length) {
        ssize_t written = send(socket_fd, message + offset,
                               (size_t)length - offset, MSG_NOSIGNAL);
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0) return false;
        offset += (size_t)written;
    }
    return true;
}

/* Write an entire benchmark byte range to the terminal's PTY. */
static bool write_all(int fd, const void *data, size_t length) {
    const unsigned char *bytes = data;
    size_t offset = 0;
    while (offset < length) {
        ssize_t written = write(fd, bytes + offset, length - offset);
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0) return false;
        offset += (size_t)written;
    }
    return true;
}

/* Connect to the controller socket while the terminal process initializes. */
static int connect_controller(const char *path) {
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;

    struct sockaddr_un address = {.sun_family = AF_UNIX};
    if (strlen(path) >= sizeof(address.sun_path)) {
        close(fd);
        errno = ENAMETOOLONG;
        return -1;
    }
    strcpy(address.sun_path, path);

    for (unsigned attempt = 0; attempt < 500; ++attempt) {
        if (connect(fd, (struct sockaddr *)&address, sizeof(address)) == 0) {
            return fd;
        }
        if (errno != ENOENT && errno != ECONNREFUSED && errno != EINTR) break;
        struct timespec pause = {.tv_sec = 0, .tv_nsec = 10000000};
        nanosleep(&pause, NULL);
    }
    close(fd);
    return -1;
}

/* Build one repeatable sample and report generation errors to the controller. */
static bool prepare_benchmark(const Benchmark *benchmark,
                              const BenchContext *context,
                              const RunOptions *options, BenchData *data,
                              char *error, size_t error_size) {
    buffer_init(&data->setup);
    buffer_init(&data->payload);
    if (benchmark->prepare(data, context, error, error_size) != 0) return false;
    if (!buffer_repeat_to_minimum(&data->payload, options->min_bytes)) {
        snprintf(error, error_size, "benchmark produced an empty or oversized payload");
        return false;
    }
    return true;
}

/* Run setup and one payload stream, resetting the terminal before each pass. */
static bool run_sample(const BenchData *data, uint64_t *duration_ns) {
    static const char reset[] = "\033c";
    if (!write_all(STDOUT_FILENO, reset, sizeof(reset) - 1)) {
        return false;
    }
    if (!write_all(STDOUT_FILENO, data->setup.data, data->setup.length)) {
        return false;
    }
    uint64_t start = monotonic_now_ns();
    if (!write_all(STDOUT_FILENO, data->payload.data, data->payload.length)) return false;
    uint64_t end = monotonic_now_ns();
    if (duration_ns != NULL) *duration_ns = end >= start ? end - start : 0;
    return true;
}

/* Order payload-write durations for percentile calculations. */
static int compare_duration(const void *left, const void *right) {
    uint64_t a = *(const uint64_t *)left;
    uint64_t b = *(const uint64_t *)right;
    return (a > b) - (a < b);
}

/* Execute all suite entries inside the terminal and bracket each workload. */
int worker_run(const AppConfig *config, const RunSelection *selection,
               const char *socket_path) {
    signal(SIGPIPE, SIG_IGN);
    bool worker_moved = selection != NULL && selection->worker_cgroup_moved;
    int socket_fd = connect_controller(socket_path);
    if (socket_fd < 0) {
        fprintf(stderr, "te-stsuite: cannot connect to controller: %s\n",
                strerror(errno));
        return 1;
    }

    if (!isatty(STDOUT_FILENO)) {
        send_event(socket_fd, "F benchmark worker stdout is not a terminal\n");
        close(socket_fd);
        return 1;
    }
    struct winsize size = {0};
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &size) != 0 ||
        size.ws_row == 0 || size.ws_col == 0) {
        size.ws_row = 24;
        size.ws_col = 80;
    }

    if (!send_event(socket_fd, "R %ld %ld %u\n", (long)getpid(),
                    (long)getppid(), worker_moved ? 1U : 0U)) {
        close(socket_fd);
        return 1;
    }
    if (selection != NULL &&
        (selection->phase == WORKER_PHASE_CHECK ||
         selection->phase == WORKER_PHASE_STARTUP)) {
        send_event(socket_fd, "D\n");
        close(socket_fd);
        return 0;
    }

    BenchContext context = {.rows = size.ws_row, .columns = size.ws_col};
    for (size_t index = 1; index < te_stsuite_benchmark_count; ++index) {
        const Benchmark *benchmark = &te_stsuite_benchmarks[index];
        if (!selection_includes_benchmark(selection, benchmark->name)) {
            continue;
        }
        BenchData data;
        char error[256] = {0};
        if (!prepare_benchmark(benchmark, &context, &config->options, &data,
                               error, sizeof(error))) {
            send_event(socket_fd, "F %s: %s\n", benchmark->name,
                       error[0] == '\0' ? "unable to prepare benchmark" : error);
            buffer_free(&data.setup);
            buffer_free(&data.payload);
            close(socket_fd);
            return 1;
        }

        if (selection != NULL && selection->phase == WORKER_PHASE_WARMUP) {
            bool warmed = true;
            for (unsigned warmup = 0; warmup < config->options.warmup; ++warmup) {
                if (!run_sample(&data, NULL)) {
                    warmed = false;
                    break;
                }
            }
            static const char reset[] = "\033c";
            if (warmed) warmed = write_all(STDOUT_FILENO, reset,
                                           sizeof(reset) - 1);
            buffer_free(&data.setup);
            buffer_free(&data.payload);
            if (!warmed) {
                send_event(socket_fd, "F %s: PTY write failed during warmup\n",
                           benchmark->name);
                close(socket_fd);
                return 1;
            }
            continue;
        }

        if (!send_event(socket_fd, "S %s %zu\n", benchmark->name,
                        data.payload.length)) {
            buffer_free(&data.setup);
            buffer_free(&data.payload);
            close(socket_fd);
            return 1;
        }

        bool ok = true;
        uint64_t deadline = monotonic_now_ns() +
                            (uint64_t)config->options.max_seconds * 1000000000ULL;
        unsigned samples = 0;
        size_t duration_capacity = 0;
        uint64_t *durations = NULL;
        const char *failure = "PTY write failed";
        while (ok && (config->options.max_samples == 0 ||
                      samples < config->options.max_samples) &&
               monotonic_now_ns() < deadline) {
            if (samples == duration_capacity) {
                size_t next_capacity = duration_capacity == 0
                                           ? 64
                                           : duration_capacity * 2;
                uint64_t *next = realloc(durations,
                                         next_capacity * sizeof(*durations));
                if (next == NULL) {
                    failure = "out of memory collecting sample times";
                    ok = false;
                    break;
                }
                durations = next;
                duration_capacity = next_capacity;
            }
            if (!run_sample(&data, &durations[samples])) {
                ok = false;
                break;
            }
            ++samples;
        }
        static const char reset[] = "\033c";
        if (ok) ok = write_all(STDOUT_FILENO, reset, sizeof(reset) - 1);
        if (ok) {
            double mean_ms = 0;
            double p90_ms = 0;
            double stddev_ms = 0;
            if (samples > 0) {
                qsort(durations, samples, sizeof(*durations), compare_duration);
                long double sum = 0;
                for (unsigned i = 0; i < samples; ++i) sum += durations[i];
                long double mean = sum / samples;
                size_t percentile_index = ((size_t)samples * 90 + 99) / 100 - 1;
                long double variance = 0;
                if (samples > 1) {
                    for (unsigned i = 0; i < samples; ++i) {
                        long double delta = (long double)durations[i] - mean;
                        variance += delta * delta;
                    }
                    variance /= samples - 1;
                }
                mean_ms = (double)(mean / 1000000.0L);
                p90_ms = (double)durations[percentile_index] / 1000000.0;
                stddev_ms = (double)(sqrtl(variance) / 1000000.0L);
            }
            send_event(socket_fd, "E %s %u %.6f %.6f %.6f\n",
                       benchmark->name, samples, mean_ms, p90_ms, stddev_ms);
        } else {
            send_event(socket_fd, "F %s: %s\n", benchmark->name, failure);
        }
        free(durations);
        buffer_free(&data.setup);
        buffer_free(&data.payload);
        if (!ok) {
            close(socket_fd);
            return 1;
        }
    }

    send_event(socket_fd, "D\n");
    close(socket_fd);
    return 0;
}
