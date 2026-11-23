#include "luatos_frame.h"

#include <assert.h>
#include <string.h>

static const uint8_t prefix[] = "\0ESP-UART:";

static size_t feed(luatos_frame_t *frame, const void *data, size_t size)
{
    const uint8_t *bytes = data;
    size_t accepted = 0;
    for (size_t i = 0; i < size; i++) {
        accepted += luatos_frame_feed(frame, bytes[i]);
    }
    return accepted;
}

int main(void)
{
    luatos_frame_t frame = {0};
    assert(feed(&frame, "ESP-UART:id\n", sizeof("ESP-UART:id\n") - 1) == 0);
    assert(feed(&frame, "\0ESP-UARx:id\n", sizeof("\0ESP-UARx:id\n") - 1) == 0);
    assert(feed(&frame, prefix, sizeof(prefix) - 1) == 0);
    assert(!luatos_frame_feed(&frame, '\n'));
    assert(feed(&frame, prefix, sizeof(prefix) - 1) == 0);
    assert(feed(&frame, "id\r\n", sizeof("id\r\n") - 1) == 0);
    assert(feed(&frame, prefix, sizeof(prefix) - 1) == 0);
    assert(feed(&frame, "\x7f\n", 2) == 0);
    assert(!frame.attempted);

    uint8_t oversized[LUATOS_MAX_LINE_LENGTH + 1];
    memset(oversized, 'a', sizeof(oversized));
    assert(feed(&frame, prefix, sizeof(prefix) - 1) == 0);
    assert(feed(&frame, oversized, sizeof(oversized)) == 0);
    assert(!luatos_frame_feed(&frame, '\n'));

    /* A new prefix replaces a partial frame, even across read boundaries. */
    assert(feed(&frame, prefix, sizeof(prefix) - 1) == 0);
    assert(feed(&frame, "abc", 3) == 0);
    assert(feed(&frame, prefix, 3) == 0);
    assert(feed(&frame, prefix + 3, sizeof(prefix) - 1 - 3) == 0);
    assert(feed(&frame, "id\n", 3) == 1);
    assert(frame.attempted);
    assert(frame.line_size == 3);
    assert(memcmp(frame.line, "id\n", 3) == 0);
    assert(feed(&frame, prefix, sizeof(prefix) - 1) == 0);
    assert(feed(&frame, "new\n", 4) == 0);

    luatos_frame_t longest = {0};
    assert(feed(&longest, prefix, sizeof(prefix) - 1) == 0);
    assert(feed(&longest, oversized, LUATOS_MAX_LINE_LENGTH) == 0);
    assert(luatos_frame_feed(&longest, '\n'));
    assert(longest.line_size == LUATOS_MAX_LINE_LENGTH + 1);
    assert(memcmp(longest.line, oversized, LUATOS_MAX_LINE_LENGTH) == 0);
    assert(longest.line[LUATOS_MAX_LINE_LENGTH] == '\n');
    return 0;
}
