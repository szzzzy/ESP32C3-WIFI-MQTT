#include <stddef.h>

#include "line_reader.h"

void line_reader_init(line_reader_t *lr, char *buf, int buf_size,
                      line_reader_cb_t on_line, void *ctx)
{
    if (lr == NULL) {
        return;
    }

    lr->buf = buf;
    lr->buf_size = buf_size;
    lr->idx = 0;
    lr->discarding = false;
    lr->on_line = on_line;
    lr->ctx = ctx;
}

line_reader_event_t line_reader_feed(line_reader_t *lr, uint8_t byte)
{
    if (lr == NULL) {
        return LINE_READER_EVENT_NONE;
    }

    /* ---- discard mode: skip until next newline ---- */
    if (lr->discarding) {
        if (byte == '\n') {
            lr->discarding = false;
            lr->idx = 0;
        }
        return LINE_READER_EVENT_NONE;
    }

    /* ---- end of line ---- */
    if (byte == '\n') {
        lr->buf[lr->idx] = '\0';

        /* strip trailing \r */
        if (lr->idx > 0 && lr->buf[lr->idx - 1] == '\r') {
            lr->buf[lr->idx - 1] = '\0';
        }

        if (lr->on_line != NULL) {
            lr->on_line(lr->buf, lr->ctx);
        }

        lr->idx = 0;
        return LINE_READER_EVENT_LINE;
    }

    /* ---- normal accumulation ---- */
    if (lr->idx < lr->buf_size - 1) {
        lr->buf[lr->idx++] = (char)byte;
        return LINE_READER_EVENT_NONE;
    }

    lr->discarding = true;
    lr->idx = 0;
    return LINE_READER_EVENT_OVERFLOW;
}
