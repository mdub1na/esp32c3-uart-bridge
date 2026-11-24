#include "d9w_recovery.h"

#include <string.h>

static const char prefix[] = "D9W1 ";
enum {
    NONCE_OFFSET = sizeof(prefix) - 1,
    BODY_OFFSET = NONCE_OFFSET + D9W_RECOVERY_NONCE_LENGTH + 1,
    LOG_WINDOW_BYTES = 65536,
    MAX_LOG_BYTES = 1048576,
};

static bool receive_line(struct d9w_recovery_line *line, uint8_t byte, size_t *size)
{
    if (byte == '\n') {
        const bool valid = !line->discard && line->length > 0;
        *size = line->length;
        line->bytes[line->length] = '\0';
        line->length = 0;
        line->discard = false;
        return valid;
    }

    if (line->discard)
        return false;
    if (line->length == sizeof(line->bytes) - 2 ||
        (byte < 0x20 && byte != '\r') || byte > 0x7e) {
        line->discard = true;
        return false;
    }
    line->bytes[line->length++] = (char)byte;
    return false;
}

static bool has_frame_prefix(const char *line, size_t size)
{
    return size > BODY_OFFSET && !memcmp(line, prefix, NONCE_OFFSET) &&
           line[BODY_OFFSET - 1] == ' ';
}

static bool valid_nonce(const char *nonce)
{
    for (size_t i = 0; i < D9W_RECOVERY_NONCE_LENGTH; i++) {
        const char c = nonce[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
            return false;
    }
    return true;
}

static void expire(struct d9w_recovery *bridge, uint64_t now_ms)
{
    if (bridge->armed && now_ms - bridge->armed_ms >= D9W_RECOVERY_SESSION_MS)
        bridge->armed = false;
}

void d9w_recovery_receive_target(struct d9w_recovery *bridge, uint8_t byte, uint64_t now_ms)
{
    expire(bridge, now_ms);
    size_t size;
    if (!receive_line(&bridge->target, byte, &size))
        return;

    char *line = bridge->target.bytes;
    if (line[size - 1] == '\r')
        line[--size] = '\0';
    if (!has_frame_prefix(line, size))
        return;

    const char *nonce = line + NONCE_OFFSET;
    const char *body = line + BODY_OFFSET;
    const bool matches = bridge->has_nonce && !memcmp(nonce, bridge->nonce, sizeof(bridge->nonce));
    if (matches && !strcmp(body, "EXIT")) {
        bridge->armed = false;
    } else if (!matches && !strcmp(body, "READY") && valid_nonce(nonce)) {
        memcpy(bridge->nonce, nonce, sizeof(bridge->nonce));
        bridge->has_nonce = true;
        bridge->armed = true;
        bridge->armed_ms = now_ms;
        bridge->requests = 0;
    }
}

static bool valid_log_offset(const char *decimal)
{
    const size_t length = strspn(decimal, "0123456789");
    if (!length || length > 6 || decimal[length] || (length > 1 && decimal[0] == '0'))
        return false;

    unsigned offset = 0;
    for (size_t i = 0; i < length; i++)
        offset = offset * 10 + (unsigned)(decimal[i] - '0');
    return offset < MAX_LOG_BYTES && offset % LOG_WINDOW_BYTES == 0;
}

static bool valid_get(const char *argument)
{
    const char *const diagnostics[] = {
        "cache.bin", "data.bin", "apex.bin", "adb.bin", "boot.bin",
        "guard-status.bin", "guard-exit.bin", "result.txt",
    };
    const char *const logs[] = {"logcat", "logcat.1", "logcat.2", "logcat.3"};
    const char *space = strchr(argument, ' ');
    if (!space) {
        for (size_t i = 0; i < sizeof(diagnostics) / sizeof(diagnostics[0]); i++)
            if (!strcmp(argument, diagnostics[i]))
                return true;
        return false;
    }

    const size_t name_size = (size_t)(space - argument);
    for (size_t i = 0; i < sizeof(logs) / sizeof(logs[0]); i++)
        if (name_size == strlen(logs[i]) && !memcmp(argument, logs[i], name_size))
            return valid_log_offset(space + 1);
    return false;
}

static bool valid_request_body(const char *body)
{
    if (!strcmp(body, "STATUS"))
        return true;
    if (!strncmp(body, "GET ", 4))
        return valid_get(body + 4);
    if (!strncmp(body, "PLAN ", 5))
        body += 5;
    else if (!strncmp(body, "RUN ", 4))
        body += 4;
    else
        return false;

    return !strcmp(body, "install") || !strcmp(body, "restore") ||
           !strcmp(body, "collect") || !strcmp(body, "reboot");
}

enum d9w_recovery_tx d9w_recovery_receive_host(struct d9w_recovery *bridge, uint8_t byte,
                                                uint64_t now_ms, size_t *query_size)
{
    expire(bridge, now_ms);
    size_t size;
    if (!receive_line(&bridge->host, byte, &size))
        return D9W_RECOVERY_TX_NONE;

    char *line = bridge->host.bytes;
    if (!bridge->armed) {
        if (!bridge->reboot_attempted && !strcmp(line, "D9W:RECOVERY")) {
            bridge->reboot_attempted = true;
            return D9W_RECOVERY_TX_REBOOT;
        }
        return D9W_RECOVERY_TX_NONE;
    }

    if (bridge->requests >= D9W_RECOVERY_REQUEST_LIMIT || !has_frame_prefix(line, size) ||
        memcmp(line + NONCE_OFFSET, bridge->nonce, sizeof(bridge->nonce)) ||
        !valid_request_body(line + BODY_OFFSET))
        return D9W_RECOVERY_TX_NONE;

    bridge->requests++;
    line[size] = '\n';
    *query_size = size + 1;
    return D9W_RECOVERY_TX_QUERY;
}
