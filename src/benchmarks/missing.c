#include "cvtebench.h"

/* Encode a Unicode scalar value as its UTF-8 byte sequence. */
static bool append_utf8(Buffer *buffer, uint32_t codepoint) {
    if (codepoint <= 0x7f) {
        return buffer_append_char(buffer, (unsigned char)codepoint);
    }
    if (codepoint <= 0x7ff) {
        return buffer_append_char(buffer, (unsigned char)(0xc0 | (codepoint >> 6))) &&
               buffer_append_char(buffer, (unsigned char)(0x80 | (codepoint & 0x3f)));
    }
    if (codepoint <= 0xffff) {
        return buffer_append_char(buffer, (unsigned char)(0xe0 | (codepoint >> 12))) &&
               buffer_append_char(buffer, (unsigned char)(0x80 | ((codepoint >> 6) & 0x3f))) &&
               buffer_append_char(buffer, (unsigned char)(0x80 | (codepoint & 0x3f)));
    }
    return buffer_append_char(buffer, (unsigned char)(0xf0 | (codepoint >> 18))) &&
           buffer_append_char(buffer, (unsigned char)(0x80 | ((codepoint >> 12) & 0x3f))) &&
           buffer_append_char(buffer, (unsigned char)(0x80 | ((codepoint >> 6) & 0x3f))) &&
           buffer_append_char(buffer, (unsigned char)(0x80 | (codepoint & 0x3f)));
}

/* Fill the terminal grid with private-use codepoints absent from most fonts. */
int bench_missing_prepare(BenchData *data, const BenchContext *context,
                          char *error, size_t error_size) {
    (void)error;
    (void)error_size;
    if (!buffer_append(&data->setup, "\033[?1049h", 8)) return -1;

    size_t cells = (size_t)context->rows * context->columns * 2;
    for (uint32_t codepoint = 58000; codepoint <= 58200; ++codepoint) {
        if (!buffer_append(&data->payload, "\033[H", 3)) return -1;
        for (size_t cell = 0; cell < cells; ++cell) {
            if (!append_utf8(&data->payload, codepoint)) return -1;
        }
    }
    return 0;
}
