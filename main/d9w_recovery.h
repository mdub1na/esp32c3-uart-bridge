#ifndef D9W_RECOVERY_H
#define D9W_RECOVERY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define D9W_RECOVERY_LINE_CAPACITY 80
#define D9W_RECOVERY_NONCE_LENGTH 32
#define D9W_RECOVERY_SESSION_MS UINT64_C(600000)
#define D9W_RECOVERY_REQUEST_LIMIT 128

enum d9w_recovery_tx {
    D9W_RECOVERY_TX_NONE,
    D9W_RECOVERY_TX_REBOOT,
    D9W_RECOVERY_TX_QUERY,
};

struct d9w_recovery_line {
    char bytes[D9W_RECOVERY_LINE_CAPACITY];
    size_t length;
    bool discard;
};

struct d9w_recovery {
    struct d9w_recovery_line target;
    struct d9w_recovery_line host;
    char nonce[D9W_RECOVERY_NONCE_LENGTH];
    uint64_t armed_ms;
    unsigned requests;
    bool has_nonce;
    bool armed;
    bool reboot_attempted;
};

void d9w_recovery_receive_target(struct d9w_recovery *bridge, uint8_t byte, uint64_t now_ms);

/* Query bytes remain in bridge->host.bytes until the next host byte. */
enum d9w_recovery_tx d9w_recovery_receive_host(struct d9w_recovery *bridge, uint8_t byte,
                                                uint64_t now_ms, size_t *query_size);

#endif
