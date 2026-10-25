#define _GNU_SOURCE
#include "cvtebench.h"
#include "tomlc17.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Copy a TOML string using its recorded length rather than an implicit size. */
static char *copy_toml_string(toml_datum_t datum) {
    if (datum.type != TOML_STRING || datum.u.str.ptr == NULL ||
        datum.u.str.len < 0) {
        return NULL;
    }
    size_t length = (size_t)datum.u.str.len;
    char *copy = malloc(length + 1);
    if (copy == NULL) return NULL;
    memcpy(copy, datum.u.str.ptr, length);
    copy[length] = '\0';
    return copy;
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

/* Release partially allocated configuration data after success or failure. */
void config_free(AppConfig *config) {
    if (config == NULL) return;
    free(config->path);
    for (size_t i = 0; i < config->terminal_count; ++i) {
        free(config->terminals[i].name);
        free(config->terminals[i].command);
        free(config->terminals[i].inline_config);
    }
    free(config->terminals);
    memset(config, 0, sizeof(*config));
}

/* Append one named terminal run, optionally paired with an inline config. */
static bool append_terminal(AppConfig *config, const char *name,
                            toml_datum_t command, const char *variant_name,
                            toml_datum_t variant_content, bool has_variant,
                            char *error, size_t error_size) {
    char *command_copy = copy_toml_string(command);
    char *name_copy = NULL;
    char *content_copy = NULL;
    size_t content_length = 0;

    if (has_variant) {
        if (variant_name != NULL && variant_name[0] != '\0') {
            if (asprintf(&name_copy, "%s/%s", name, variant_name) < 0) {
                name_copy = NULL;
            }
        }
        content_copy = copy_toml_string(variant_content);
        if (variant_content.type == TOML_STRING &&
            variant_content.u.str.len >= 0) {
            content_length = (size_t)variant_content.u.str.len;
        }
    } else {
        name_copy = strdup(name);
    }

    if (command_copy == NULL || name_copy == NULL || command_copy[0] == '\0' ||
        (has_variant && content_copy == NULL)) {
        snprintf(error, error_size,
                 "terminal '%s' needs a nonempty cmd and each config variant needs a nonempty name and string content",
                 name);
        free(command_copy);
        free(name_copy);
        free(content_copy);
        return false;
    }

    TerminalConfig *terminals = realloc(
        config->terminals,
        (config->terminal_count + 1) * sizeof(*config->terminals));
    if (terminals == NULL) {
        snprintf(error, error_size, "out of memory while loading terminal list");
        free(command_copy);
        free(name_copy);
        free(content_copy);
        return false;
    }
    config->terminals = terminals;
    config->terminals[config->terminal_count++] = (TerminalConfig){
        .name = name_copy,
        .command = command_copy,
        .inline_config = content_copy,
        .inline_config_length = content_length,
    };
    return true;
}

/* Parse root runtime options and each named terminal table in source order. */
bool config_load(const char *path, AppConfig *config, char *error,
                 size_t error_size) {
    memset(config, 0, sizeof(*config));
    config->options.min_bytes = 1048576;
    config->options.warmup = 1;
    config->options.max_seconds = 10;
    config->options.max_samples = 0;
    config->options.startup_timeout_seconds = 30;
    config->monitor_backend = MONITOR_BACKEND_CGROUPV2;

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

    unsigned long long min_bytes = config->options.min_bytes;
    unsigned long long warmup = config->options.warmup;
    unsigned long long max_seconds = config->options.max_seconds;
    unsigned long long max_samples = config->options.max_samples;
    unsigned long long startup_timeout = config->options.startup_timeout_seconds;
    toml_datum_t monitor_name = toml_get(result.toptab, "monitor");
    if (monitor_name.type != TOML_UNKNOWN) {
        char *name = copy_toml_string(monitor_name);
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
    bool valid = read_option(result.toptab, "min_bytes", SIZE_MAX, &min_bytes,
                             error, error_size) &&
                 read_option(result.toptab, "warmup", UINT_MAX, &warmup,
                             error, error_size) &&
                 read_option(result.toptab, "max_seconds", UINT_MAX, &max_seconds,
                             error, error_size) &&
                 read_option(result.toptab, "max_samples", UINT_MAX, &max_samples,
                             error, error_size) &&
                 read_option(result.toptab, "startup_timeout_seconds", UINT_MAX,
                             &startup_timeout, error, error_size);
    if (valid && (min_bytes == 0 || max_seconds == 0 || startup_timeout == 0)) {
        snprintf(error, error_size,
                 "min_bytes, max_seconds, and startup_timeout_seconds must be positive");
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
    config->options.startup_timeout_seconds = (unsigned)startup_timeout;

    bool parsed_terminals = false;
    for (int i = 0; i < result.toptab.u.tab.size; ++i) {
        const char *key = result.toptab.u.tab.key[i];
        toml_datum_t value = result.toptab.u.tab.value[i];
        if (value.type != TOML_TABLE) {
            continue;
        }
        parsed_terminals = true;
        toml_datum_t command = toml_get(value, "cmd");
        toml_datum_t variants = toml_get(value, "configs");
        if (variants.type == TOML_UNKNOWN) {
            if (!append_terminal(config, key, command, NULL,
                                 (toml_datum_t){0}, false, error,
                                 error_size)) {
                toml_free(result);
                config_free(config);
                return false;
            }
            continue;
        }
        if (variants.type != TOML_TABLE || variants.u.tab.size <= 0) {
            snprintf(error, error_size,
                     "terminal '%s' configs must be a nonempty table of inline config strings",
                     key);
            toml_free(result);
            config_free(config);
            return false;
        }
        for (int variant_index = 0; variant_index < variants.u.tab.size;
             ++variant_index) {
            if (!append_terminal(config, key, command,
                                 variants.u.tab.key[variant_index],
                                 variants.u.tab.value[variant_index], true,
                                 error, error_size)) {
                toml_free(result);
                config_free(config);
                return false;
            }
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
