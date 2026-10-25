#define _GNU_SOURCE
#include "cvtebench.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct {
    const char **items;
    size_t count;
    size_t capacity;
} SelectorList;

/* Show the run modes, selectors, workload names, and measurement flow. */
static void print_usage(const char *program) {
    printf("Usage: %s CONFIG.toml [OPTIONS]\n\n", program);
    printf("Runs terminal emulator benchmarks on modern Linux from named TOML entries.\n");
    printf("It launches each selected terminal in turn, pins it to CPU 0, and starts\n");
    printf("a worker in its PTY. It samples process-tree CPU time and RSS for startup\n");
    printf("and each selected workload, and records PTY write times. Results are\n");
    printf("printed per benchmark, summarized per terminal and collectively, and\n");
    printf("saved as timestamped text and JSONL logs in the current directory.\n\n");
    printf("Options:\n");
    printf("  -h, --help               Show this help\n");
    printf("      --validate           Check the TOML and selectors without running\n");
    printf("  -t, --terminal NAME      Run only this terminal; repeat to select more\n");
    printf("  -b, --benchmark NAME     Run only this workload; repeat to select more\n\n");
    printf("A terminal name selects all its config variants; a name such as\n");
    printf("kitty/compact selects one variant. Workloads:\n  ");
    size_t line_length = 2;
    for (size_t i = 0; i < cvtebench_benchmark_count; ++i) {
        size_t name_length = strlen(cvtebench_benchmarks[i].name);
        size_t separator_length = i == 0 ? 0 : 2;
        if (line_length + separator_length + name_length > 78) {
            printf("\n  ");
            line_length = 2;
            separator_length = 0;
        }
        if (separator_length != 0) {
            printf(", ");
            line_length += separator_length;
        }
        printf("%s", cvtebench_benchmarks[i].name);
        line_length += name_length;
    }
    printf("\nWith no selectors, all configured terminals and workloads run.\n");
}

/* Store one selector from argv without copying its process-lifetime string. */
static bool selector_list_add(SelectorList *list, const char *name) {
    if (list->count == list->capacity) {
        size_t capacity = list->capacity == 0 ? 4 : list->capacity * 2;
        const char **items = realloc(list->items, capacity * sizeof(*items));
        if (items == NULL) return false;
        list->items = items;
        list->capacity = capacity;
    }
    list->items[list->count++] = name;
    return true;
}

/* Release the selector pointer array after the run has completed. */
static void selector_list_free(SelectorList *list) {
    free(list->items);
    memset(list, 0, sizeof(*list));
}

/* Resolve this executable so terminal templates can launch the worker again. */
static char *self_path(void) {
    char path[4096];
    ssize_t length = readlink("/proc/self/exe", path, sizeof(path) - 1);
    if (length < 0 || (size_t)length >= sizeof(path) - 1) return NULL;
    path[length] = '\0';
    return strdup(path);
}

/* Parse the required config path and dispatch validation, control, or worker mode. */
int main(int argc, char **argv) {
    const char *config_path = NULL;
    const char *socket_path = NULL;
    bool run = false;
    bool validate = false;
    bool help = false;
    SelectorList terminals = {0};
    SelectorList benchmarks = {0};
    bool arguments_valid = true;

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--run") == 0) {
            run = true;
        } else if (strcmp(argv[i], "--validate") == 0) {
            validate = true;
        } else if (strcmp(argv[i], "--terminal") == 0 ||
                   strcmp(argv[i], "-t") == 0 ||
                   strcmp(argv[i], "--benchmark") == 0 ||
                   strcmp(argv[i], "-b") == 0) {
            bool is_terminal = strcmp(argv[i], "--terminal") == 0 ||
                               strcmp(argv[i], "-t") == 0;
            if (++i >= argc || argv[i][0] == '-') {
                fprintf(stderr, "cvtebench: %s needs a name\n",
                        is_terminal ? "--terminal/-t" : "--benchmark/-b");
                arguments_valid = false;
                break;
            }
            SelectorList *list = is_terminal ? &terminals : &benchmarks;
            if (!selector_list_add(list, argv[i])) {
                fprintf(stderr, "cvtebench: out of memory storing selectors\n");
                arguments_valid = false;
                break;
            }
        } else if (strcmp(argv[i], "--worker-socket") == 0) {
            if (++i >= argc) {
                fprintf(stderr, "cvtebench: --worker-socket needs a path\n");
                arguments_valid = false;
                break;
            }
            socket_path = argv[i];
        } else if (strcmp(argv[i], "--help") == 0 ||
                   strcmp(argv[i], "-h") == 0) {
            help = true;
        } else if (argv[i][0] == '-') {
            fprintf(stderr, "cvtebench: unknown option '%s'\n", argv[i]);
            arguments_valid = false;
            break;
        } else if (config_path == NULL) {
            config_path = argv[i];
        } else {
            fprintf(stderr, "cvtebench: only one TOML configuration is accepted\n");
            arguments_valid = false;
            break;
        }
    }

    if (!arguments_valid) {
        print_usage(argv[0]);
        selector_list_free(&terminals);
        selector_list_free(&benchmarks);
        return 2;
    }
    if (help) {
        print_usage(argv[0]);
        selector_list_free(&terminals);
        selector_list_free(&benchmarks);
        return 0;
    }
    if (config_path == NULL) {
        fprintf(stderr, "cvtebench: an input TOML configuration is required\n");
        print_usage(argv[0]);
        selector_list_free(&terminals);
        selector_list_free(&benchmarks);
        return 2;
    }
    if (socket_path != NULL && (!run || validate)) {
        fprintf(stderr, "cvtebench: --worker-socket requires --run\n");
        selector_list_free(&terminals);
        selector_list_free(&benchmarks);
        return 2;
    }
    if (run && validate) {
        fprintf(stderr, "cvtebench: --run and --validate cannot be combined\n");
        selector_list_free(&terminals);
        selector_list_free(&benchmarks);
        return 2;
    }

    RunSelection selection = {
        .terminal_names = terminals.items,
        .terminal_count = terminals.count,
        .benchmark_names = benchmarks.items,
        .benchmark_count = benchmarks.count,
    };

    AppConfig config;
    char error[512] = {0};
    if (!config_load(config_path, &config, error, sizeof(error))) {
        fprintf(stderr, "cvtebench: %s\n", error);
        selector_list_free(&terminals);
        selector_list_free(&benchmarks);
        return 2;
    }
    if (!selection_validate(&config, &selection, error, sizeof(error))) {
        fprintf(stderr, "cvtebench: %s\n", error);
        config_free(&config);
        selector_list_free(&terminals);
        selector_list_free(&benchmarks);
        return 2;
    }

    int result = 0;
    if (socket_path != NULL) {
        result = worker_run(&config, &selection, socket_path);
    } else if (validate) {
        printf("Validated %s with %zu terminal configuration(s).\n",
               config.path, config.terminal_count);
    } else {
        char *executable_path = self_path();
        if (executable_path == NULL) {
            fprintf(stderr, "cvtebench: cannot resolve the running executable path\n");
            result = 1;
        } else {
            result = controller_run(&config, &selection, executable_path);
            free(executable_path);
        }
    }

    config_free(&config);
    selector_list_free(&terminals);
    selector_list_free(&benchmarks);
    return result;
}
