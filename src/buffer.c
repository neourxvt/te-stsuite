#define _GNU_SOURCE
#include "cvtebench.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Initialize an empty benchmark byte buffer. */
void buffer_init(Buffer *buffer) {
    buffer->data = NULL;
    buffer->length = 0;
    buffer->capacity = 0;
}

/* Release all storage owned by a benchmark buffer. */
void buffer_free(Buffer *buffer) {
    free(buffer->data);
    buffer_init(buffer);
}

/* Reserve enough capacity while checking for size arithmetic overflow. */
static bool buffer_reserve(Buffer *buffer, size_t extra) {
    if (extra > SIZE_MAX - buffer->length) {
        return false;
    }
    size_t needed = buffer->length + extra;
    if (needed <= buffer->capacity) {
        return true;
    }

    size_t capacity = buffer->capacity == 0 ? 4096 : buffer->capacity;
    while (capacity < needed) {
        if (capacity > SIZE_MAX / 2) {
            capacity = needed;
            break;
        }
        capacity *= 2;
    }

    unsigned char *data = realloc(buffer->data, capacity);
    if (data == NULL) {
        return false;
    }
    buffer->data = data;
    buffer->capacity = capacity;
    return true;
}

/* Append an arbitrary byte range to the buffer. */
bool buffer_append(Buffer *buffer, const void *data, size_t length) {
    if (!buffer_reserve(buffer, length)) {
        return false;
    }
    memcpy(buffer->data + buffer->length, data, length);
    buffer->length += length;
    return true;
}

/* Append one byte to a benchmark payload. */
bool buffer_append_char(Buffer *buffer, unsigned char value) {
    return buffer_append(buffer, &value, 1);
}

/* Fill the buffer with a repeated byte while reserving space only once. */
bool buffer_append_repeat(Buffer *buffer, unsigned char value, size_t count) {
    if (!buffer_reserve(buffer, count)) {
        return false;
    }
    memset(buffer->data + buffer->length, value, count);
    buffer->length += count;
    return true;
}

/* Append formatted bytes, sizing the temporary string before allocation. */
bool buffer_appendf(Buffer *buffer, const char *format, ...) {
    va_list args;
    va_start(args, format);
    va_list copy;
    va_copy(copy, args);
    int count = vsnprintf(NULL, 0, format, copy);
    va_end(copy);
    if (count < 0 || !buffer_reserve(buffer, (size_t)count + 1)) {
        va_end(args);
        return false;
    }
    vsnprintf((char *)buffer->data + buffer->length, (size_t)count + 1,
              format, args);
    va_end(args);
    buffer->length += (size_t)count;
    return true;
}

/* Append a binary fixture without treating its contents as a C string. */
bool buffer_load_file(Buffer *buffer, const char *path) {
    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        return false;
    }

    unsigned char chunk[16384];
    size_t count;
    bool ok = true;
    while ((count = fread(chunk, 1, sizeof(chunk), file)) > 0) {
        if (!buffer_append(buffer, chunk, count)) {
            ok = false;
            break;
        }
    }
    if (ferror(file)) {
        ok = false;
    }
    fclose(file);
    return ok;
}

/* Repeat one generated sample enough times to match vtebench's size floor. */
bool buffer_repeat_to_minimum(Buffer *buffer, size_t minimum) {
    if (buffer->length == 0) {
        return false;
    }
    if (buffer->length >= minimum) {
        return true;
    }

    size_t original_length = buffer->length;
    size_t repeats = (minimum - 1) / original_length;
    if (repeats > SIZE_MAX / original_length) {
        return false;
    }
    size_t extra = repeats * original_length;
    if (!buffer_reserve(buffer, extra)) {
        return false;
    }
    for (size_t i = 0; i < repeats; ++i) {
        memcpy(buffer->data + buffer->length, buffer->data, original_length);
        buffer->length += original_length;
    }
    return true;
}
