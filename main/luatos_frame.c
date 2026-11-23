#include "luatos_frame.h"

static const uint8_t frame_prefix[] = "\0ESP-UART:";
#define FRAME_PREFIX_LENGTH (sizeof(frame_prefix) - 1)

bool luatos_frame_feed(luatos_frame_t *frame, uint8_t byte)
{
    if (frame->attempted) {
        return false;
    }

    if (byte == frame_prefix[0]) {
        frame->prefix_size = 1;
        frame->line_size = 0;
        return false;
    }
    if (frame->prefix_size == 0) {
        return false;
    }
    if (frame->prefix_size < FRAME_PREFIX_LENGTH) {
        if (byte == frame_prefix[frame->prefix_size]) {
            frame->prefix_size++;
        } else {
            frame->prefix_size = 0;
        }
        return false;
    }

    if (byte == '\n' && frame->line_size > 0) {
        frame->line[frame->line_size++] = byte;
        frame->attempted = true;
        return true;
    }
    if (byte < 0x20 || byte > 0x7e || frame->line_size == LUATOS_MAX_LINE_LENGTH) {
        frame->prefix_size = 0;
        return false;
    }

    frame->line[frame->line_size++] = byte;
    return false;
}
