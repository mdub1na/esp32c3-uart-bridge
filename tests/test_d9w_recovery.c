#include "d9w_recovery.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#define FIRST "D9W1 0123456789abcdef0123456789abcdef "
#define SECOND "D9W1 fedcba9876543210fedcba9876543210 "

static void target(struct d9w_recovery *bridge, const char *line, uint64_t now)
{
    for (size_t i = 0; line[i]; i++)
        d9w_recovery_receive_target(bridge, (uint8_t)line[i], now);
}

static enum d9w_recovery_tx host(struct d9w_recovery *bridge, const char *line, uint64_t now)
{
    size_t forwarded = 0;
    const size_t length = strlen(line);
    enum d9w_recovery_tx result = D9W_RECOVERY_TX_NONE;
    for (size_t i = 0; i < length; i++) {
        result = d9w_recovery_receive_host(bridge, (uint8_t)line[i], now, &forwarded);
        assert(i + 1 == length || result == D9W_RECOVERY_TX_NONE);
    }
    if (result == D9W_RECOVERY_TX_QUERY) {
        assert(forwarded == length);
        assert(!memcmp(bridge->host.bytes, line, length));
    }
    return result;
}

static void test_recovery_and_session_state(void)
{
    struct d9w_recovery bridge = {0};
    assert(host(&bridge, FIRST "STATUS\n", 0) == D9W_RECOVERY_TX_NONE);
    assert(host(&bridge, "D9W:RECOVERY now\n", 0) == D9W_RECOVERY_TX_NONE);
    assert(host(&bridge, "D9W:RECOVERY\n", 0) == D9W_RECOVERY_TX_REBOOT);
    assert(host(&bridge, "D9W:RECOVERY\n", 0) == D9W_RECOVERY_TX_NONE);

    target(&bridge, FIRST "CHUNK 0 "
           "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\n"
           "D9W1 Z123456789abcdef0123456789abcdef READY\r\n", 1);
    assert(host(&bridge, FIRST "STATUS\n", 1) == D9W_RECOVERY_TX_NONE);
    target(&bridge, FIRST "READY\r\n", 2);
    assert(host(&bridge, SECOND "STATUS\n", 2) == D9W_RECOVERY_TX_NONE);
    target(&bridge, SECOND "EXIT\n", 3);
    assert(host(&bridge, FIRST "STATUS\n", 3) == D9W_RECOVERY_TX_QUERY);
    target(&bridge, FIRST "EXIT\r\n" FIRST "READY\n", 4);
    assert(host(&bridge, FIRST "STATUS\n", 4) == D9W_RECOVERY_TX_NONE);
    target(&bridge, SECOND "READY\n", 5);
    assert(host(&bridge, FIRST "STATUS\n", 5) == D9W_RECOVERY_TX_NONE);
    assert(host(&bridge, SECOND "STATUS\n", 5) == D9W_RECOVERY_TX_QUERY);

    memset(&bridge, 0, sizeof(bridge));
    target(&bridge, FIRST "READY\n", 0);
    assert(host(&bridge, "D9W:RECOVERY\n", 1) == D9W_RECOVERY_TX_NONE);
    target(&bridge, FIRST "EXIT\n", 2);
    assert(host(&bridge, "D9W:RECOVERY\n", 2) == D9W_RECOVERY_TX_REBOOT);
}

static void test_fixed_requests(void)
{
    const char *const allowed[] = {
        FIRST "GET logcat 0\n", FIRST "GET logcat.1 65536\n",
        FIRST "GET logcat.2 524288\n", FIRST "GET logcat.3 983040\n",
        FIRST "PLAN install\n", FIRST "RUN install\n",
        FIRST "PLAN restore\n", FIRST "RUN restore\n",
        FIRST "PLAN collect\n", FIRST "RUN collect\n",
        FIRST "PLAN reboot\n", FIRST "RUN reboot\n",
    };
    const char *const rejected[] = {
        FIRST "GET logcat\n", FIRST "GET logcat.4 0\n", FIRST "GET logcat.10 0\n",
        FIRST "GET ../logcat 0\n", FIRST "GET logcat.1/ 0\n", FIRST "GET other.txt\n",
        FIRST "GET logcat 00\n", FIRST "GET logcat -1\n", FIRST "GET logcat +0\n",
        FIRST "GET logcat 1\n", FIRST "GET logcat 1048576\n",
        FIRST "GET logcat 0\r\n", FIRST "GET logcat 0 \n", FIRST "GET logcat 0 x\n",
        FIRST "GET logcat 6553\t6\n", FIRST "GET  logcat 0\n",
        FIRST "GET logcat 9999999999999999999999999999999999999999999999999\n",
        FIRST "GET boot.bin 0\n", FIRST "GET result.txt \n",
        FIRST "RUN anything\n", FIRST "RUN reboot shell\n", FIRST "PLAN restore;reboot\n",
        FIRST "STATUS \n", FIRST "STATUS\r\n", FIRST "SH\n",
    };

    struct d9w_recovery bridge = {0};
    target(&bridge, FIRST "READY\n", 1);
    for (size_t i = 0; i < sizeof(rejected) / sizeof(rejected[0]); i++)
        assert(host(&bridge, rejected[i], 2) == D9W_RECOVERY_TX_NONE);
    assert(bridge.requests == 0);

    for (size_t i = 0; i < sizeof(allowed) / sizeof(allowed[0]); i++)
        assert(host(&bridge, allowed[i], 3) == D9W_RECOVERY_TX_QUERY);
    assert(bridge.requests == sizeof(allowed) / sizeof(allowed[0]));
}

static void test_full_log_windows_and_limits(void)
{
    const char *const diagnostics[] = {
        FIRST "GET cache.bin\n", FIRST "GET data.bin\n", FIRST "GET apex.bin\n",
        FIRST "GET adb.bin\n", FIRST "GET boot.bin\n", FIRST "GET guard-status.bin\n",
        FIRST "GET guard-exit.bin\n", FIRST "GET result.txt\n",
    };
    struct d9w_recovery bridge = {0};
    target(&bridge, FIRST "READY\n", 0);
    for (size_t i = 0; i < sizeof(diagnostics) / sizeof(diagnostics[0]); i++)
        assert(host(&bridge, diagnostics[i], 1) == D9W_RECOVERY_TX_QUERY);

    /* Sixteen 64 KiB windows need about 220 seconds at 115200 with hex frames. */
    for (unsigned index = 0; index < 16; index++) {
        char request[D9W_RECOVERY_LINE_CAPACITY];
        const int length = snprintf(request, sizeof(request), FIRST "GET logcat %u\n",
                                    index * 65536u);
        assert(length > 0 && (size_t)length < sizeof(request));
        assert(host(&bridge, request, 1000 + index * 14000) == D9W_RECOVERY_TX_QUERY);
    }
    assert(bridge.requests == 24);

    for (unsigned i = bridge.requests; i < D9W_RECOVERY_REQUEST_LIMIT; i++)
        assert(host(&bridge, FIRST "STATUS\n", 230000) == D9W_RECOVERY_TX_QUERY);
    target(&bridge, FIRST "READY\r\n", 231000);
    assert(host(&bridge, FIRST "STATUS\n", 231000) == D9W_RECOVERY_TX_NONE);

    target(&bridge, SECOND "READY\n", 232000);
    assert(host(&bridge, SECOND "STATUS\n", 232000 + D9W_RECOVERY_SESSION_MS - 1) ==
           D9W_RECOVERY_TX_QUERY);
    target(&bridge, SECOND "READY\n", 232000 + D9W_RECOVERY_SESSION_MS);
    assert(host(&bridge, SECOND "STATUS\n", 232000 + D9W_RECOVERY_SESSION_MS) ==
           D9W_RECOVERY_TX_NONE);
}

int main(void)
{
    test_recovery_and_session_state();
    test_fixed_requests();
    test_full_log_windows_and_limits();
    puts("D9W recovery frame tests passed");
    return 0;
}
