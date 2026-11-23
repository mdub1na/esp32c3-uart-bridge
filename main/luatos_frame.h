#ifndef LUATOS_FRAME_H
#define LUATOS_FRAME_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define LUATOS_MAX_LINE_LENGTH 128

typedef struct {
    size_t prefix_size;
    size_t line_size;
    uint8_t line[LUATOS_MAX_LINE_LENGTH + 1];
    bool attempted;
} luatos_frame_t;

/* Use zero initialization. On the first complete frame, line and line_size
 * contain the UART payload including its trailing newline.
 */
bool luatos_frame_feed(luatos_frame_t *frame, uint8_t byte);

#endif
