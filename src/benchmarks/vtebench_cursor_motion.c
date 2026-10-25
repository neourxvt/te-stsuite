#include "te-stsuite.h"

/* Emit the nested cursor spiral used by the original cursor-motion workload. */
int bench_cursor_motion_prepare(BenchData *data, const BenchContext *context,
                                char *error, size_t error_size) {
    (void)error;
    (void)error_size;
    unsigned rows = context->rows;
    unsigned columns = context->columns;

    for (unsigned letter = 0; letter < 26; ++letter) {
        int column_start = 1;
        int column_end = (int)columns;
        int line_start = 1;
        int line_end = (int)rows;
        while (column_start <= column_end && line_start <= line_end) {
            int column = column_start;
            int line = line_start;
            while (column < column_end) {
                if (!buffer_appendf(&data->payload, "\033[%d;%dH%c", line,
                                    column, 'A' + (int)letter)) return -1;
                ++column;
            }
            while (line < line_end) {
                if (!buffer_appendf(&data->payload, "\033[%d;%dH%c", line,
                                    column, 'A' + (int)letter)) return -1;
                ++line;
            }
            while (column > column_start) {
                if (!buffer_appendf(&data->payload, "\033[%d;%dH%c", line,
                                    column, 'A' + (int)letter)) return -1;
                --column;
            }
            while (line > line_start) {
                if (!buffer_appendf(&data->payload, "\033[%d;%dH%c", line,
                                    column, 'A' + (int)letter)) return -1;
                --line;
            }
            ++column_start;
            --column_end;
            ++line_start;
            --line_end;
        }
    }
    return 0;
}
