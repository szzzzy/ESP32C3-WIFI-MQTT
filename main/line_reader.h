#ifndef LINE_READER_H
#define LINE_READER_H

#include <stdbool.h>
#include <stdint.h>

/**
 * @brief Callback invoked when a complete line has been accumulated.
 *
 * @param line  NUL-terminated line (trailing \r already stripped).
 * @param ctx   Opaque user context passed to line_reader_init().
 */
typedef void (*line_reader_cb_t)(const char *line, void *ctx);

/**
 * @brief Events returned by line_reader_feed().
 */
typedef enum {
    LINE_READER_EVENT_NONE     = 0, /**< Byte accumulated, no line ready yet. */
    LINE_READER_EVENT_LINE     = 1, /**< Complete line received, callback invoked. */
    LINE_READER_EVENT_OVERFLOW = 2, /**< Buffer overflow, now discarding until next \n. */
} line_reader_event_t;

/**
 * @brief Reusable line accumulator state (opaque — callers only use via API).
 *
 * Feed bytes one at a time via line_reader_feed(). When a \n byte arrives,
 * the buffer contents (minus trailing \r) are passed to on_line().
 * Lines exceeding buf_size are discarded until the next \n.
 */
typedef struct {
    char *buf;
    int buf_size;
    int idx;
    bool discarding;
    line_reader_cb_t on_line;
    void *ctx;
} line_reader_t;

/**
 * @brief Initialise a line_reader instance.
 *
 * @param lr        Pointer to caller-owned line_reader_t.
 * @param buf       Caller-provided buffer for accumulating a single line.
 * @param buf_size  Size of buf in bytes.
 * @param on_line   Callback invoked for each complete line (must not be NULL).
 * @param ctx       User context forwarded to on_line.
 */
void line_reader_init(line_reader_t *lr, char *buf, int buf_size,
                      line_reader_cb_t on_line, void *ctx);

/**
 * @brief Feed a single byte into the line accumulator.
 *
 * When a complete line is detected, on_line() is called synchronously
 * before this function returns.
 *
 * @return Event indicating what happened with this byte.
 *         LINE_READER_EVENT_LINE — a complete line was delivered to on_line().
 *         LINE_READER_EVENT_OVERFLOW — buffer overflow; discarding until next \n.
 *         LINE_READER_EVENT_NONE — byte consumed, no line or overflow this cycle.
 */
line_reader_event_t line_reader_feed(line_reader_t *lr, uint8_t byte);

#endif
