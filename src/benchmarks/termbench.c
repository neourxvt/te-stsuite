/* TermBench V2 workloads, copyright (c) 2021 Casey Muratori, MIT licensed.
 * See ATTRIBUTIONS.md and LICENSES/MIT-termbench.txt for attribution and terms.
 */
#include "te-stsuite.h"

#include <stdio.h>

/* The runner repeats text chunks to min_bytes; RGB frames cover a byte cycle. */
enum {
    TERMBENCH_TEXT_BYTES = 64 * 1024,
    TERMBENCH_COLOR_FRAMES = 256,
};

/* Use a local fixed-seed generator so selection and warmup cannot change bytes. */
static int prepare_text(BenchData *data, bool newlines,
                        char *error, size_t error_size) {
    uint32_t random = 1;
    unsigned symbols = newlines ? 27 : 26;

    for (size_t i = 0; i < TERMBENCH_TEXT_BYTES; ++i) {
        random ^= random << 13;
        random ^= random >> 17;
        random ^= random << 5;
        unsigned pick = random % symbols;
        unsigned char glyph = pick == 26 ? '\n' : (unsigned char)('a' + pick);
        if (!buffer_append_char(&data->payload, glyph)) {
            snprintf(error, error_size, "cannot prepare termbench text payload");
            return -1;
        }
    }
    return 0;
}

/* Precompute RGB gradients over the visible grid, outside PTY write timing. */
static int prepare_colors(BenchData *data, const BenchContext *context,
                          bool background, char *error, size_t error_size) {
    for (unsigned frame = 0; frame < TERMBENCH_COLOR_FRAMES; ++frame) {
        for (unsigned row = 0; row < context->rows; ++row) {
            if (!buffer_appendf(&data->payload, "\033[%u;1H", row + 1)) {
                goto allocation_failed;
            }
            for (unsigned column = 0; column < context->columns; ++column) {
                unsigned red = frame;
                unsigned green = (frame + row) & 0xff;
                unsigned blue = (frame + row + column) & 0xff;
                /* Preserve the original a-through-y glyph cycle. */
                unsigned char glyph = 'a' + (frame + row + column) % ('z' - 'a');
                if (background &&
                    !buffer_appendf(&data->payload, "\033[48;2;%u;%u;%um",
                                    blue, green, red)) {
                    goto allocation_failed;
                }
                if (!buffer_appendf(&data->payload, "\033[38;2;%u;%u;%um%c",
                                    red, green, blue, glyph)) {
                    goto allocation_failed;
                }
            }
        }
    }
    return 0;

allocation_failed:
    snprintf(error, error_size, "cannot prepare termbench RGB payload");
    return -1;
}

/* Mix lowercase ASCII with randomly placed newlines, as in ManyLine. */
int bench_termbench_many_line_prepare(BenchData *data,
                                     const BenchContext *context,
                                     char *error, size_t error_size) {
    (void)context;
    return prepare_text(data, true, error, error_size);
}

/* Leave all line breaks to the terminal's automatic wrapping, as in LongLine. */
int bench_termbench_long_line_prepare(BenchData *data,
                                     const BenchContext *context,
                                     char *error, size_t error_size) {
    (void)context;
    return prepare_text(data, false, error, error_size);
}

/* Change each cell's RGB foreground while retaining the default background. */
int bench_termbench_fg_per_char_prepare(BenchData *data,
                                       const BenchContext *context,
                                       char *error, size_t error_size) {
    return prepare_colors(data, context, false, error, error_size);
}

/* Set separate RGB background and foreground sequences for every cell. */
int bench_termbench_fgbg_per_char_prepare(BenchData *data,
                                         const BenchContext *context,
                                         char *error, size_t error_size) {
    return prepare_colors(data, context, true, error, error_size);
}
