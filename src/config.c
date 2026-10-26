#define _GNU_SOURCE
#include "te-stsuite.h"
#include "process_isolation.h"
#include "tomlc17.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Keep variable definitions as views into the TOML document during loading. */
typedef struct {
    toml_datum_t table;
} TemplateVariables;

/* Load named string values used to expand {NAME} references in TOML strings. */
static bool read_template_variables(toml_datum_t root,
                                    TemplateVariables *variables,
                                    char *error, size_t error_size) {
    toml_datum_t table = toml_get(root, "variables");
    if (table.type == TOML_UNKNOWN) return true;
    if (table.type != TOML_TABLE) {
        snprintf(error, error_size, "variables must be a table of strings");
        return false;
    }

    for (int i = 0; i < table.u.tab.size; ++i) {
        const char *name = table.u.tab.key[i];
        size_t name_length = (size_t)table.u.tab.len[i];
        toml_datum_t value = table.u.tab.value[i];
        bool valid_name = name_length > 0 &&
            ((name[0] >= 'A' && name[0] <= 'Z') ||
             (name[0] >= 'a' && name[0] <= 'z') || name[0] == '_');
        for (size_t j = 1; j < name_length && valid_name; ++j) {
            unsigned char byte = (unsigned char)name[j];
            valid_name = (byte >= 'A' && byte <= 'Z') ||
                         (byte >= 'a' && byte <= 'z') ||
                         (byte >= '0' && byte <= '9') || byte == '_';
        }
        if (name_length == 0 || !valid_name ||
            (name_length == 3 && memcmp(name, "CMD", 3) == 0) ||
            (name_length == 3 && memcmp(name, "CFG", 3) == 0) ||
            (name_length == 7 && memcmp(name, "CFG_DIR", 7) == 0) ||
            value.type != TOML_STRING || value.u.str.ptr == NULL ||
            value.u.str.len < 0 ||
            memchr(value.u.str.ptr, '\0', (size_t)value.u.str.len) != NULL) {
            snprintf(error, error_size,
                     "variables need identifier names other than CMD, CFG, or CFG_DIR and NUL-free string values");
            return false;
        }
    }
    variables->table = table;
    return true;
}

/* Find a variable only when the complete brace token matches. */
static toml_datum_t find_template_variable(
    const TemplateVariables *variables, const char *name, size_t name_length) {
    for (int i = 0; i < variables->table.u.tab.size; ++i) {
        size_t variable_name_length = (size_t)variables->table.u.tab.len[i];
        if (variable_name_length == name_length &&
            memcmp(variables->table.u.tab.key[i], name, name_length) == 0) {
            return variables->table.u.tab.value[i];
        }
    }
    return (toml_datum_t){0};
}

/* Expand known {NAME} tokens while preserving unrelated terminal placeholders. */
static char *template_string(const char *source, size_t source_length,
                             const TemplateVariables *variables,
                             size_t *result_length, char *error,
                             size_t error_size) {
    if (source == NULL || source_length > INT_MAX ||
        memchr(source, '\0', source_length) != NULL) {
        snprintf(error, error_size, "configuration strings must not contain NUL bytes");
        return NULL;
    }

    Buffer expanded;
    buffer_init(&expanded);
    for (size_t i = 0; i < source_length;) {
        if (source[i] == '{') {
            const char *close = memchr(source + i + 1, '}', source_length - i - 1);
            if (close != NULL) {
                size_t name_length = (size_t)(close - (source + i + 1));
                toml_datum_t variable = find_template_variable(
                    variables, source + i + 1, name_length);
                if (variable.type == TOML_STRING) {
                    if (!buffer_append(&expanded, variable.u.str.ptr,
                                       (size_t)variable.u.str.len)) goto memory_error;
                    i += name_length + 2;
                    continue;
                }
            }
        }
        if (!buffer_append_char(&expanded, (unsigned char)source[i++])) {
            goto memory_error;
        }
    }

    char *result = malloc(expanded.length + 1);
    if (result == NULL) goto memory_error;
    if (expanded.length > 0) memcpy(result, expanded.data, expanded.length);
    result[expanded.length] = '\0';
    *result_length = expanded.length;
    buffer_free(&expanded);
    return result;

memory_error:
    snprintf(error, error_size, "out of memory while expanding configuration string");
    buffer_free(&expanded);
    return NULL;
}

/* Copy and template a TOML string using its recorded length. */
static char *template_toml_string(toml_datum_t datum,
                                  const TemplateVariables *variables,
                                  size_t *result_length, char *error,
                                  size_t error_size) {
    if (datum.type != TOML_STRING || datum.u.str.ptr == NULL ||
        datum.u.str.len < 0) {
        return NULL;
    }
    return template_string(datum.u.str.ptr, (size_t)datum.u.str.len,
                           variables, result_length, error, error_size);
}

/* Read a one-line version command and its optional captured-line limit. */
static bool read_terminal_version(toml_datum_t version,
                                  const TemplateVariables *variables,
                                  char **command, bool *has_head,
                                  unsigned *head, const char *terminal,
                                  char *error, size_t error_size) {
    *command = NULL;
    *has_head = false;
    *head = 0;
    if (version.type == TOML_UNKNOWN) return true;
    if (version.type != TOML_TABLE ||
        (version.flag & TOML_FLAG_INLINED) == 0) {
        snprintf(error, error_size,
                 "terminal '%s' version must be a single-line inline table with cmd and optional head",
                 terminal);
        return false;
    }

    for (int i = 0; i < version.u.tab.size; ++i) {
        const char *key = version.u.tab.key[i];
        if (strcmp(key, "cmd") != 0 && strcmp(key, "head") != 0) {
            snprintf(error, error_size,
                     "terminal '%s' version only accepts cmd and head",
                     terminal);
            return false;
        }
    }

    toml_datum_t command_datum = toml_get(version, "cmd");
    size_t command_length = 0;
    char *command_copy = template_toml_string(command_datum, variables,
                                              &command_length, error,
                                              error_size);
    if (command_copy == NULL || command_length == 0 ||
        command_datum.lineno != version.lineno ||
        memchr(command_copy, '\n', command_length) != NULL ||
        memchr(command_copy, '\r', command_length) != NULL) {
        snprintf(error, error_size,
                 "terminal '%s' version needs a nonempty single-line cmd",
                 terminal);
        free(command_copy);
        return false;
    }

    toml_datum_t head_datum = toml_get(version, "head");
    if (head_datum.type != TOML_UNKNOWN) {
        if (head_datum.type != TOML_INT64 || head_datum.u.int64 < 0 ||
            (uint64_t)head_datum.u.int64 > UINT_MAX) {
            snprintf(error, error_size,
                     "terminal '%s' version head must be a nonnegative integer",
                     terminal);
            free(command_copy);
            return false;
        }
        if (head_datum.lineno != version.lineno) {
            snprintf(error, error_size,
                     "terminal '%s' version cmd and head must be on one line",
                     terminal);
            free(command_copy);
            return false;
        }
        *has_head = true;
        *head = (unsigned)head_datum.u.int64;
    }
    *command = command_copy;
    return true;
}

/* Apply an optional unsigned integer setting after checking its TOML type. */
static bool read_option(toml_datum_t table, const char *key,
                        unsigned long long maximum, unsigned long long *value,
                        char *error, size_t error_size) {
    toml_datum_t datum = toml_get(table, key);
    if (datum.type == TOML_UNKNOWN) return true;
    if (datum.type != TOML_INT64 || datum.u.int64 < 0 ||
        (unsigned long long)datum.u.int64 > maximum) {
        snprintf(error, error_size, "configuration key '%s' must be a nonnegative integer", key);
        return false;
    }
    *value = (unsigned long long)datum.u.int64;
    return true;
}

/* Serialize a TOML resource table as one X resource specification per row. */
static bool serialize_xresources(toml_datum_t table, char **content,
                                 size_t *content_length, char *error,
                                 size_t error_size,
                                 const TemplateVariables *variables) {
    Buffer serialized;
    buffer_init(&serialized);
    for (int i = 0; i < table.u.tab.size; ++i) {
        const char *key = table.u.tab.key[i];
        int key_length = table.u.tab.len[i];
        toml_datum_t value = table.u.tab.value[i];
        char scalar[64];
        char *expanded_value = NULL;
        const char *value_text = NULL;
        size_t value_length = 0;

        if (key == NULL || key_length <= 0 ||
            memchr(key, '\0', (size_t)key_length) != NULL ||
            memchr(key, ':', (size_t)key_length) != NULL ||
            memchr(key, '\n', (size_t)key_length) != NULL ||
            memchr(key, '\r', (size_t)key_length) != NULL) {
            snprintf(error, error_size,
                     "xresources keys must be nonempty resource names without colons or line breaks");
            buffer_free(&serialized);
            return false;
        }

        switch (value.type) {
        case TOML_STRING:
            if (value.u.str.ptr == NULL || value.u.str.len < 0) {
                snprintf(error, error_size,
                         "xresources values must be single-line TOML scalars");
                buffer_free(&serialized);
                return false;
            }
            expanded_value = template_string(value.u.str.ptr,
                                             (size_t)value.u.str.len,
                                             variables, &value_length, error,
                                             error_size);
            if (expanded_value == NULL) {
                buffer_free(&serialized);
                return false;
            }
            if (memchr(expanded_value, '\n', value_length) != NULL ||
                memchr(expanded_value, '\r', value_length) != NULL) {
                snprintf(error, error_size,
                         "xresources values must be single-line TOML scalars");
                free(expanded_value);
                buffer_free(&serialized);
                return false;
            }
            value_text = expanded_value;
            break;
        case TOML_INT64: {
            int length = snprintf(scalar, sizeof(scalar), "%lld",
                                  (long long)value.u.int64);
            if (length < 0 || (size_t)length >= sizeof(scalar)) goto memory_error;
            value_text = scalar;
            value_length = (size_t)length;
            break;
        }
        case TOML_FP64: {
            int length = snprintf(scalar, sizeof(scalar), "%.17g",
                                  value.u.fp64);
            if (length < 0 || (size_t)length >= sizeof(scalar)) goto memory_error;
            value_text = scalar;
            value_length = (size_t)length;
            break;
        }
        case TOML_BOOLEAN:
            value_text = value.u.boolean ? "true" : "false";
            value_length = value.u.boolean ? 4 : 5;
            break;
        default:
            snprintf(error, error_size,
                     "xresources values must be strings, integers, floats, or booleans");
            buffer_free(&serialized);
            return false;
        }

        bool appended = buffer_append(&serialized, key, (size_t)key_length) &&
            buffer_append(&serialized, ": ", 2) &&
            buffer_append(&serialized, value_text, value_length) &&
            buffer_append_char(&serialized, '\n');
        free(expanded_value);
        if (!appended) {
            goto memory_error;
        }
    }

    *content = malloc(serialized.length + 1);
    if (*content == NULL) goto memory_error;
    if (serialized.length > 0) {
        memcpy(*content, serialized.data, serialized.length);
    }
    (*content)[serialized.length] = '\0';
    *content_length = serialized.length;
    buffer_free(&serialized);
    return true;

memory_error:
    snprintf(error, error_size, "out of memory while serializing xresources");
    buffer_free(&serialized);
    return false;
}

/* Release partially allocated configuration data after success or failure. */
void config_free(AppConfig *config) {
    if (config == NULL) return;
    free(config->path);
    for (size_t i = 0; i < config->terminal_count; ++i) {
        free(config->terminals[i].name);
        free(config->terminals[i].emulator_name);
        free(config->terminals[i].command);
        free(config->terminals[i].version_command);
        free(config->terminals[i].inline_config);
        for (size_t j = 0; j < config->terminals[i].env_count; ++j) {
            free(config->terminals[i].env[j].name);
            free(config->terminals[i].env[j].value);
        }
        free(config->terminals[i].env);
        free(config->terminals[i].xresources);
    }
    free(config->terminals);
    for (size_t i = 0; i < config->benchmark_count; ++i) {
        free(config->benchmarks[i].name);
    }
    free(config->benchmarks);
    memset(config, 0, sizeof(*config));
}

/* Preserve literal environment values and validate optional config references. */
static bool read_terminal_env(toml_datum_t table, TerminalConfig *terminal,
                              const TemplateVariables *variables,
                              char *error, size_t error_size) {
    if (table.type == TOML_UNKNOWN) return true;
    if (table.type != TOML_TABLE) {
        snprintf(error, error_size, "terminal '%s' env must be a table",
                 terminal->name);
        return false;
    }
    terminal->env = calloc((size_t)table.u.tab.size, sizeof(*terminal->env));
    if (table.u.tab.size > 0 && terminal->env == NULL) {
        snprintf(error, error_size, "out of memory while loading terminal env");
        return false;
    }
    for (int i = 0; i < table.u.tab.size; ++i) {
        const char *name = table.u.tab.key[i];
        int length = table.u.tab.len[i];
        toml_datum_t value = table.u.tab.value[i];
        if (length <= 0 || memchr(name, '=', (size_t)length) != NULL ||
            memchr(name, '\0', (size_t)length) != NULL ||
            value.type != TOML_STRING || value.u.str.ptr == NULL ||
            value.u.str.len < 0) {
            snprintf(error, error_size,
                     "terminal '%s' env needs nonempty names without '=' and string values without NUL bytes",
                     terminal->name);
            return false;
        }
        size_t value_length = 0;
        char *text = template_toml_string(value, variables, &value_length,
                                          error, error_size);
        if (text == NULL) return false;
        (void)value_length;
        bool config_ref = strstr(text, "{CFG}") != NULL ||
                          strstr(text, "{CFG_DIR}") != NULL;
        if (config_ref && (terminal->inline_config == NULL ||
            (strcmp(text, "{CFG}") != 0 && strcmp(text, "{CFG_DIR}") != 0))) {
            snprintf(error, error_size,
                     "terminal '%s' env config references must be exactly {CFG} or {CFG_DIR} and require configs",
                     terminal->name);
            free(text);
            return false;
        }
        TerminalEnv *entry = &terminal->env[terminal->env_count++];
        entry->name = strndup(name, (size_t)length);
        entry->value = text;
        if (entry->name == NULL) {
            snprintf(error, error_size, "out of memory while loading terminal env");
            return false;
        }
    }
    return true;
}

/* Append one named terminal run, optionally paired with an inline config. */
static bool append_terminal(AppConfig *config, const char *name,
                            toml_datum_t command, const char *variant_name,
                            toml_datum_t variant_content, bool has_variant,
                            const char *version_command,
                            bool has_version_head, unsigned version_head,
                            bool silence_output,
                            toml_datum_t xresources, toml_datum_t env,
                            const TemplateVariables *variables,
                            char *error, size_t error_size) {
    size_t command_length = 0;
    char *command_copy = template_toml_string(command, variables,
                                              &command_length, error,
                                              error_size);
    (void)command_length;
    char *name_copy = NULL;
    char *emulator_name_copy = strdup(name);
    char *content_copy = NULL;
    char *version_copy = version_command == NULL
                             ? NULL
                             : strdup(version_command);
    size_t content_length = 0;
    char *xresources_copy = NULL;
    size_t xresources_length = 0;

    if (xresources.type == TOML_TABLE &&
        !serialize_xresources(xresources, &xresources_copy,
                              &xresources_length, error, error_size,
                              variables)) {
        free(command_copy);
        free(emulator_name_copy);
        free(version_copy);
        return false;
    }

    if (has_variant) {
        if (variant_name != NULL && variant_name[0] != '\0') {
            if (asprintf(&name_copy, "%s/%s", name, variant_name) < 0) {
                name_copy = NULL;
            }
        }
        content_copy = template_toml_string(variant_content, variables,
                                            &content_length, error,
                                            error_size);
    } else {
        name_copy = strdup(name);
    }

    if (command_copy == NULL || name_copy == NULL ||
        emulator_name_copy == NULL || command_copy[0] == '\0' ||
        (has_variant && content_copy == NULL) ||
        (version_command != NULL && version_copy == NULL)) {
        snprintf(error, error_size,
                 "terminal '%s' needs a nonempty cmd and each config variant needs a nonempty name and string content",
                 name);
        free(command_copy);
        free(name_copy);
        free(emulator_name_copy);
        free(content_copy);
        free(version_copy);
        free(xresources_copy);
        return false;
    }

    TerminalConfig *terminals = realloc(
        config->terminals,
        (config->terminal_count + 1) * sizeof(*config->terminals));
    if (terminals == NULL) {
        snprintf(error, error_size, "out of memory while loading terminal list");
        free(command_copy);
        free(name_copy);
        free(emulator_name_copy);
        free(content_copy);
        free(version_copy);
        free(xresources_copy);
        return false;
    }
    config->terminals = terminals;
    config->terminals[config->terminal_count++] = (TerminalConfig){
        .name = name_copy,
        .emulator_name = emulator_name_copy,
        .command = command_copy,
        .version_command = version_copy,
        .version_head = version_head,
        .has_version_head = has_version_head,
        .silence_output = silence_output,
        .inline_config = content_copy,
        .inline_config_length = content_length,
        .xresources = xresources_copy,
        .xresources_length = xresources_length,
    };
    return read_terminal_env(env, &config->terminals[config->terminal_count - 1],
                             variables, error, error_size);
}

/* Append an independently configured run while preserving suite order. */
static bool append_benchmark(AppConfig *config, const Benchmark *benchmark,
                             const char *name, const RunOptions *options,
                             char *error, size_t error_size) {
    char *name_copy = strdup(name);
    if (name_copy == NULL) goto memory_error;
    BenchmarkRun *runs = realloc(config->benchmarks,
                                 (config->benchmark_count + 1) * sizeof(*runs));
    if (runs == NULL) {
        free(name_copy);
        goto memory_error;
    }
    config->benchmarks = runs;
    config->benchmarks[config->benchmark_count++] = (BenchmarkRun){
        .benchmark = benchmark,
        .name = name_copy,
        .options = *options,
    };
    return true;

memory_error:
    snprintf(error, error_size, "out of memory while loading benchmark runs");
    return false;
}

/* Read one Typometer configuration without inheriting another variant's values. */
static bool read_typometer_options(toml_datum_t table, RunOptions *options,
                                   char *error, size_t error_size) {
    static const char *const keys[] = {
        "count", "delay_ms", "asynchronous", "pause_every", "pause_ms",
    };
    if (table.type != TOML_TABLE) {
        snprintf(error, error_size, "each typometer configuration must be a table");
        return false;
    }
    for (int i = 0; i < table.u.tab.size; ++i) {
        const char *key = table.u.tab.key[i];
        bool known = false;
        for (size_t j = 0; j < sizeof(keys) / sizeof(*keys); ++j) {
            if ((size_t)table.u.tab.len[i] == strlen(keys[j]) &&
                strcmp(key, keys[j]) == 0) known = true;
        }
        if (!known) {
            snprintf(error, error_size, "unknown typometer option '%s'", key);
            return false;
        }
    }
    toml_datum_t asynchronous = toml_get(table, "asynchronous");
    if (asynchronous.type != TOML_UNKNOWN && asynchronous.type != TOML_BOOLEAN) {
        snprintf(error, error_size, "asynchronous must be a boolean");
        return false;
    }
    unsigned long long count = options->typometer_count;
    unsigned long long delay = options->typometer_delay_ms;
    unsigned long long pause_every = options->typometer_pause_every;
    unsigned long long pause = options->typometer_pause_ms;
    if (!read_option(table, "count", UINT_MAX, &count, error, error_size) ||
        !read_option(table, "delay_ms", UINT_MAX, &delay, error, error_size) ||
        !read_option(table, "pause_every", UINT_MAX, &pause_every, error, error_size) ||
        !read_option(table, "pause_ms", UINT_MAX, &pause, error, error_size)) {
        return false;
    }
    if (count == 0) {
        snprintf(error, error_size, "count must be positive");
        return false;
    }
    options->typometer_count = (unsigned)count;
    options->typometer_delay_ms = (unsigned)delay;
    options->typometer_pause_every = (unsigned)pause_every;
    options->typometer_pause_ms = (unsigned)pause;
    options->typometer_asynchronous =
        asynchronous.type == TOML_BOOLEAN && asynchronous.u.boolean;
    return true;
}

/* Expand Typometer variants in TOML order at their normal place in the suite. */
static bool read_benchmarks(toml_datum_t root, AppConfig *config,
                            char *error, size_t error_size) {
    static const char *const legacy_keys[] = {
        "typometer_count", "typometer_delay_ms", "typometer_asynchronous",
        "typometer_pause_every", "typometer_pause_ms",
    };
    for (size_t i = 0; i < sizeof(legacy_keys) / sizeof(*legacy_keys); ++i) {
        if (toml_get(root, legacy_keys[i]).type != TOML_UNKNOWN) {
            snprintf(error, error_size,
                     "root typometer_* options have moved to [typometer.NAME]; use count, delay_ms, asynchronous, pause_every, and pause_ms");
            return false;
        }
    }
    toml_datum_t variants = toml_get(root, "typometer");
    if (variants.type != TOML_UNKNOWN &&
        (variants.type != TOML_TABLE || variants.u.tab.size == 0)) {
        snprintf(error, error_size,
                 "typometer must be a nonempty table of named configurations such as [typometer.basic]");
        return false;
    }
    for (size_t i = 0; i < te_stsuite_benchmark_count; ++i) {
        const Benchmark *benchmark = &te_stsuite_benchmarks[i];
        if (!benchmark_is_typometer(benchmark->name) ||
            variants.type == TOML_UNKNOWN) {
            if (!append_benchmark(config, benchmark, benchmark->name,
                                   &config->options, error, error_size)) return false;
            continue;
        }
        for (int j = 0; j < variants.u.tab.size; ++j) {
            const char *key = variants.u.tab.key[j];
            int length = variants.u.tab.len[j];
            char name[128];
            /* Worker events carry one whitespace-free name of at most 127 bytes. */
            bool valid_name = length > 0 &&
                (size_t)length + sizeof("typometer/") - 1 < sizeof(name);
            for (int k = 0; k < length && valid_name; ++k) {
                unsigned char byte = (unsigned char)key[k];
                if (byte <= ' ' || byte == 127 || byte == '/') valid_name = false;
            }
            if (!valid_name) {
                snprintf(error, error_size,
                         "typometer config names must be nonempty, at most 117 bytes, and contain no spaces, control characters, or slashes");
                return false;
            }
            snprintf(name, sizeof(name), "typometer/%s", key);
            RunOptions options = config->options;
            char detail[256];
            if (!read_typometer_options(variants.u.tab.value[j], &options,
                                         detail, sizeof(detail))) {
                snprintf(error, error_size, "%s: %s", name, detail);
                return false;
            }
            if (!append_benchmark(config, benchmark, name, &options,
                                   error, error_size)) return false;
        }
    }
    return true;
}

/* Parse runtime options, named Typometer runs, and terminals in source order. */
bool config_load(const char *path, AppConfig *config, char *error,
                 size_t error_size) {
    memset(config, 0, sizeof(*config));
    config->options.min_bytes = 1048576;
    config->options.warmup = 1;
    config->options.max_seconds = 10;
    config->options.max_samples = 0;
    config->options.startup_min_seconds = 3;
    config->options.startup_min_samples = 10;
    config->options.startup_timeout_seconds = 30;
    config->options.typometer_count = 100;
    config->options.typometer_pause_ms = 1000;
    config->monitor_backend = MONITOR_BACKEND_CGROUPV2;
    /* Taskset works without cpuset controller delegation. */
    config->process_isolation = PROCESS_ISOLATION_TASKSET;

    char *absolute_path = realpath(path, NULL);
    if (absolute_path == NULL) {
        snprintf(error, error_size, "cannot resolve configuration '%s'", path);
        return false;
    }
    config->path = absolute_path;

    toml_result_t result = toml_parse_file_ex(path);
    if (!result.ok) {
        snprintf(error, error_size, "cannot parse TOML: %s", result.errmsg);
        toml_free(result);
        config_free(config);
        return false;
    }

    TemplateVariables variables = {0};
    if (!read_template_variables(result.toptab, &variables, error,
                                 error_size)) {
        toml_free(result);
        config_free(config);
        return false;
    }

    unsigned long long min_bytes = config->options.min_bytes;
    unsigned long long warmup = config->options.warmup;
    unsigned long long max_seconds = config->options.max_seconds;
    unsigned long long max_samples = config->options.max_samples;
    unsigned long long startup_min_seconds =
        config->options.startup_min_seconds;
    unsigned long long startup_min_samples =
        config->options.startup_min_samples;
    unsigned long long startup_timeout = config->options.startup_timeout_seconds;
    toml_datum_t monitor_name = toml_get(result.toptab, "monitor");
    if (monitor_name.type != TOML_UNKNOWN) {
        size_t name_length = 0;
        char *name = template_toml_string(monitor_name, &variables,
                                          &name_length, error, error_size);
        (void)name_length;
        if (name == NULL ||
            !monitor_backend_parse(name, &config->monitor_backend)) {
            snprintf(error, error_size,
                     "configuration key 'monitor' must be 'polling' or 'cgroupv2'");
            free(name);
            toml_free(result);
            config_free(config);
            return false;
        }
        free(name);
    }
    toml_datum_t isolation_name = toml_get(result.toptab, "process_isolation");
    if (isolation_name.type != TOML_UNKNOWN) {
        size_t name_length = 0;
        char *name = template_toml_string(isolation_name, &variables,
                                          &name_length, error, error_size);
        (void)name_length;
        if (name == NULL ||
            !process_isolation_parse(name, &config->process_isolation)) {
            snprintf(error, error_size,
                     "configuration key 'process_isolation' must be 'cgroupv2' or 'taskset'");
            free(name);
            toml_free(result);
            config_free(config);
            return false;
        }
        free(name);
    }
    bool valid = read_option(result.toptab, "min_bytes", SIZE_MAX, &min_bytes,
                             error, error_size) &&
                 read_option(result.toptab, "warmup", UINT_MAX, &warmup,
                             error, error_size) &&
                 read_option(result.toptab, "max_seconds", UINT_MAX, &max_seconds,
                             error, error_size) &&
                 read_option(result.toptab, "max_samples", UINT_MAX, &max_samples,
                             error, error_size) &&
                 read_option(result.toptab, "startup_min_seconds", UINT_MAX,
                             &startup_min_seconds, error, error_size) &&
                 read_option(result.toptab, "startup_min_samples", UINT_MAX,
                             &startup_min_samples, error, error_size) &&
                 read_option(result.toptab, "startup_timeout_seconds", UINT_MAX,
                             &startup_timeout, error, error_size);
    if (valid && (min_bytes == 0 || max_seconds == 0 ||
                  startup_min_seconds == 0 || startup_min_samples == 0 ||
                  startup_timeout == 0)) {
        snprintf(error, error_size,
                 "min_bytes, max_seconds, startup_min_seconds, startup_min_samples, and startup_timeout_seconds must be positive");
        valid = false;
    }
    if (!valid) {
        toml_free(result);
        config_free(config);
        return false;
    }
    config->options.min_bytes = (size_t)min_bytes;
    config->options.warmup = (unsigned)warmup;
    config->options.max_seconds = (unsigned)max_seconds;
    config->options.max_samples = (unsigned)max_samples;
    config->options.startup_min_seconds = (unsigned)startup_min_seconds;
    config->options.startup_min_samples = (unsigned)startup_min_samples;
    config->options.startup_timeout_seconds = (unsigned)startup_timeout;
    if (!read_benchmarks(result.toptab, config, error, error_size)) {
        toml_free(result);
        config_free(config);
        return false;
    }

    bool parsed_terminals = false;
    for (int i = 0; i < result.toptab.u.tab.size; ++i) {
        const char *key = result.toptab.u.tab.key[i];
        toml_datum_t value = result.toptab.u.tab.value[i];
        if (strcmp(key, "typometer") == 0 ||
            strcmp(key, "variables") == 0) continue;
        if (value.type != TOML_TABLE) {
            continue;
        }
        parsed_terminals = true;
        toml_datum_t command = toml_get(value, "cmd");
        toml_datum_t variants = toml_get(value, "configs");
        toml_datum_t xresources = toml_get(value, "xresources");
        toml_datum_t env = toml_get(value, "env");
        toml_datum_t silence_output = toml_get(value, "silence_output");
        if (silence_output.type != TOML_UNKNOWN &&
            silence_output.type != TOML_BOOLEAN) {
            snprintf(error, error_size,
                     "terminal '%s' silence_output must be a boolean", key);
            toml_free(result);
            config_free(config);
            return false;
        }
        if (xresources.type != TOML_UNKNOWN &&
            xresources.type != TOML_TABLE) {
            snprintf(error, error_size,
                     "terminal '%s' xresources must be a table", key);
            toml_free(result);
            config_free(config);
            return false;
        }
        if (variants.type != TOML_UNKNOWN &&
            (variants.type != TOML_TABLE || variants.u.tab.size <= 0)) {
            snprintf(error, error_size,
                     "terminal '%s' configs must be a nonempty table of inline config strings",
                     key);
            toml_free(result);
            config_free(config);
            return false;
        }

        char *version_command = NULL;
        bool has_version_head = false;
        unsigned version_head = 0;
        if (!read_terminal_version(toml_get(value, "version"), &variables,
                                   &version_command, &has_version_head,
                                   &version_head, key, error, error_size)) {
            toml_free(result);
            config_free(config);
            return false;
        }
        if (variants.type == TOML_UNKNOWN) {
            bool appended = append_terminal(
                config, key, command, NULL, (toml_datum_t){0}, false,
                version_command, has_version_head, version_head,
                silence_output.type == TOML_BOOLEAN &&
                    silence_output.u.boolean,
                xresources, env, &variables, error, error_size);
            free(version_command);
            if (!appended) {
                toml_free(result);
                config_free(config);
                return false;
            }
            continue;
        }
        bool appended_all = true;
        for (int variant_index = 0; variant_index < variants.u.tab.size;
             ++variant_index) {
            if (!append_terminal(config, key, command,
                                 variants.u.tab.key[variant_index],
                                 variants.u.tab.value[variant_index], true,
                                 version_command, has_version_head,
                                 version_head,
                                 silence_output.type == TOML_BOOLEAN &&
                                     silence_output.u.boolean,
                                 xresources, env, &variables,
                                 error, error_size)) {
                appended_all = false;
                break;
            }
        }
        free(version_command);
        if (!appended_all) {
            toml_free(result);
            config_free(config);
            return false;
        }
    }
    if (!parsed_terminals || config->terminal_count == 0) {
        snprintf(error, error_size,
                 "configuration must define one or more named terminal tables such as [kitty]");
        toml_free(result);
        config_free(config);
        return false;
    }

    toml_free(result);
    return true;
}
