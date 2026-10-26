#define _GNU_SOURCE
#include "te-stsuite.h"
#include "process_isolation.h"

#include <errno.h>
#include <math.h>
#include <stdarg.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

/* Send a complete control event through the worker's FIFO. */
static bool send_event(int control_fd, const char *format, ...) {
    char message[512];
    va_list args;
    va_start(args, format);
    int length = vsnprintf(message, sizeof(message), format, args);
    va_end(args);
    if (length < 0 || (size_t)length >= sizeof(message)) return false;

    size_t offset = 0;
    while (offset < (size_t)length) {
        ssize_t written = write(control_fd, message + offset,
                                (size_t)length - offset);
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

/* Open the controller FIFO after its reader has been created. */
static int open_controller_pipe(const char *path) {
    for (unsigned attempt = 0; attempt < 500; ++attempt) {
        int fd = open(path, O_WRONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd >= 0) {
            int flags = fcntl(fd, F_GETFL);
            if (flags < 0 || fcntl(fd, F_SETFL, flags & ~O_NONBLOCK) < 0) {
                int saved_errno = errno;
                close(fd);
                errno = saved_errno;
                return -1;
            }
            return fd;
        }
        if (errno != ENOENT && errno != ENXIO && errno != EINTR) break;
        struct timespec pause = {.tv_sec = 0, .tv_nsec = 10000000};
        nanosleep(&pause, NULL);
    }
    return -1;
}

/* Let window-manager startup resizes settle before fixing the workload grid. */
static bool wait_for_terminal_size(struct winsize *size, unsigned timeout_seconds) {
    const uint64_t quiet_ns = 100000000ULL;
    uint64_t now = monotonic_now_ns();
    uint64_t stable_since = now;
    uint64_t deadline = now + (uint64_t)timeout_seconds * 1000000000ULL;
    *size = (struct winsize){0};

    while (now < deadline) {
        struct winsize current = {0};
        int status = ioctl(STDOUT_FILENO, TIOCGWINSZ, &current);
        if (status < 0 && errno != EINTR) return false;
        now = monotonic_now_ns();
        if (status < 0 || current.ws_row == 0 || current.ws_col == 0 ||
            current.ws_row != size->ws_row || current.ws_col != size->ws_col) {
            *size = current;
            stable_since = now;
        } else if (now - stable_since >= quiet_ns) {
            return true;
        }

        /* Use elapsed time so interrupted sleeps cannot shorten settling. */
        struct timespec pause = {.tv_sec = 0, .tv_nsec = 10000000};
        if (nanosleep(&pause, NULL) != 0 && errno != EINTR) return false;
        now = monotonic_now_ns();
    }
    errno = ETIMEDOUT;
    return false;
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

/* Summarize either payload writes or active benchmark latency samples. */
static bool report_samples(int control_fd, const char *name, uint64_t *durations,
                            unsigned samples) {
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
    return send_event(control_fd, "E %s %u %.6f %.6f %.6f\n",
               name, samples, mean_ms, p90_ms, stddev_ms);
}

/* A calibrated active benchmark announces its own accounting boundary. */
typedef struct {
    int control_fd;
    const char *name;
} ActiveBenchmark;

/* Keep the private worker protocol identical for active and payload runs. */
static bool begin_active_benchmark(void *user) {
    ActiveBenchmark *active = user;
    return send_event(active->control_fd, "S %s 0\n", active->name);
}

/* Execute all suite entries inside the terminal and bracket each workload. */
int worker_run(const AppConfig *config, const RunSelection *selection,
               const char *control_path) {
    signal(SIGPIPE, SIG_IGN);
    bool worker_moved = selection != NULL && selection->worker_cgroup_moved;
    int control_fd = open_controller_pipe(control_path);
    if (control_fd < 0) {
        fprintf(stderr, "te-stsuite: cannot open controller control pipe: %s\n",
                strerror(errno));
        return 1;
    }

    /* Reapply taskset affinity after a terminal may have reset its child mask. */
    if (config->process_isolation == PROCESS_ISOLATION_TASKSET &&
        !process_isolation_pin_pid(getpid(), 0)) {
        send_event(control_fd, "F cannot pin benchmark worker to CPU 0: %s\n",
                   strerror(errno));
        close(control_fd);
        return 1;
    }

    bool checking_isolation = selection != NULL &&
                              selection->phase == WORKER_PHASE_CHECK;
    sigset_t check_ack_signal;
    if (checking_isolation) {
        /* Keep the check worker alive until the controller inspects its PID. */
        sigemptyset(&check_ack_signal);
        sigaddset(&check_ack_signal, SIGUSR1);
        if (sigprocmask(SIG_BLOCK, &check_ack_signal, NULL) != 0) {
            send_event(control_fd,
                       "F cannot prepare process isolation check: %s\n",
                       strerror(errno));
            close(control_fd);
            return 1;
        }
    }

    if (!isatty(STDOUT_FILENO)) {
        send_event(control_fd, "F benchmark worker stdout is not a terminal\n");
        close(control_fd);
        return 1;
    }
    if (!send_event(control_fd, "R %ld %ld %u\n", (long)getpid(),
                    (long)getppid(), worker_moved ? 1U : 0U)) {
        close(control_fd);
        return 1;
    }
    if (selection != NULL &&
        (selection->phase == WORKER_PHASE_CHECK ||
         selection->phase == WORKER_PHASE_STARTUP)) {
        if (checking_isolation) {
            int received_signal = 0;
            int wait_error;
            do {
                wait_error = sigwait(&check_ack_signal, &received_signal);
            } while (wait_error == EINTR);
            if (wait_error != 0 || received_signal != SIGUSR1) {
                send_event(control_fd,
                           "F process isolation check acknowledgement failed: %s\n",
                           strerror(wait_error == 0 ? EIO : wait_error));
                close(control_fd);
                return 1;
            }
        }
        send_event(control_fd, "D\n");
        close(control_fd);
        return 0;
    }

    /* Startup timing and isolation checks do not need a settled workload grid. */
    struct winsize size = {0};
    if (!wait_for_terminal_size(&size, config->options.startup_timeout_seconds)) {
        send_event(control_fd, "F cannot determine stable terminal dimensions: %s\n",
                   strerror(errno));
        close(control_fd);
        return 1;
    }

    BenchContext context = {.rows = size.ws_row, .columns = size.ws_col};
    for (size_t index = 1; index < config->benchmark_count; ++index) {
        const BenchmarkRun *run = &config->benchmarks[index];
        const Benchmark *benchmark = run->benchmark;
        const RunOptions *options = &run->options;
        if (!selection_includes_benchmark(selection, run->name)) {
            continue;
        }
        if (benchmark->execute != NULL) {
            bool warmup = selection != NULL &&
                          selection->phase == WORKER_PHASE_WARMUP;
            unsigned passes = warmup ? options->warmup : 1;
            bool ok = true;
            for (unsigned pass = 0; pass < passes && ok; ++pass) {
                BenchSamples samples = {0};
                ActiveBenchmark active = {control_fd, run->name};
                char error[256] = {0};
                ok = benchmark->execute(&context, options, warmup,
                                         begin_active_benchmark, &active,
                                         &samples, error, sizeof(error));
                /* Relay pixel measurements separately from timing samples. */
                if (ok && !warmup && samples.has_cell_dimensions) {
                    ok = send_event(control_fd, "D %s %u %u\n", run->name,
                                    samples.cell_width_px,
                                    samples.cell_height_px);
                }
                if (ok && !warmup) {
                    ok = report_samples(control_fd, run->name,
                                         samples.values, samples.count);
                }
                free(samples.values);
                if (!ok) send_event(control_fd, "F %s: %s\n", run->name,
                                    error[0] ? error : "active benchmark failed");
            }
            if (!ok) {
                close(control_fd);
                return 1;
            }
            continue;
        }
        BenchData data;
        char error[256] = {0};
        if (!prepare_benchmark(benchmark, &context, options, &data,
                               error, sizeof(error))) {
            send_event(control_fd, "F %s: %s\n", run->name,
                       error[0] == '\0' ? "unable to prepare benchmark" : error);
            buffer_free(&data.setup);
            buffer_free(&data.payload);
            close(control_fd);
            return 1;
        }

        if (selection != NULL && selection->phase == WORKER_PHASE_WARMUP) {
            bool warmed = true;
            for (unsigned warmup = 0; warmup < options->warmup; ++warmup) {
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
                send_event(control_fd, "F %s: PTY write failed during warmup\n",
                           run->name);
                close(control_fd);
                return 1;
            }
            continue;
        }

        if (!send_event(control_fd, "S %s %zu\n", run->name,
                        data.payload.length)) {
            buffer_free(&data.setup);
            buffer_free(&data.payload);
            close(control_fd);
            return 1;
        }

        bool ok = true;
        uint64_t deadline = monotonic_now_ns() +
                            (uint64_t)options->max_seconds * 1000000000ULL;
        unsigned samples = 0;
        size_t duration_capacity = 0;
        uint64_t *durations = NULL;
        const char *failure = "PTY write failed";
        while (ok && (options->max_samples == 0 ||
                      samples < options->max_samples) &&
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
            ok = report_samples(control_fd, run->name, durations, samples);
        } else {
            send_event(control_fd, "F %s: %s\n", run->name, failure);
        }
        free(durations);
        buffer_free(&data.setup);
        buffer_free(&data.payload);
        if (!ok) {
            close(control_fd);
            return 1;
        }
    }

    send_event(control_fd, "D\n");
    close(control_fd);
    return 0;
}
