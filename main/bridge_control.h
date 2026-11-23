#ifndef BRIDGE_CONTROL_H
#define BRIDGE_CONTROL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define BRIDGE_ARM_HOLD_MS UINT64_C(1500)
#define BRIDGE_RELEASE_DEBOUNCE_MS UINT64_C(30)
#define BRIDGE_PENDING_CAPACITY 512U

typedef enum {
    BRIDGE_RECEIVE_ONLY,
    BRIDGE_ARMED,
    BRIDGE_FAULT,
} bridge_mode_t;

typedef struct {
    bridge_mode_t mode;
    bool press_active;
    bool release_confirmed;
    bool release_pending;
    bool hold_candidate;
    uint64_t pressed_at_ms;
    uint64_t released_at_ms;
} bridge_control_t;

typedef struct {
    uint8_t bytes[BRIDGE_PENDING_CAPACITY];
    size_t length;
    size_t offset;
} bridge_pending_t;

void bridge_control_init(bridge_control_t *control, bool button_pressed);
void bridge_control_update(bridge_control_t *control, uint64_t now_ms,
                           bool button_pressed, bool usb_connected, bool fault);
/* Only the explicit internal-loopback image calls this once per boot. */
void bridge_control_test_arm(bridge_control_t *control, bool usb_connected);
void bridge_pending_clear(bridge_pending_t *pending);
size_t bridge_pending_remaining(const bridge_pending_t *pending);
bool bridge_pending_consume(bridge_pending_t *pending, size_t count);
/* Bytes observed while unarmed are deliberately discarded, never staged. */
bool bridge_pending_stage(bridge_pending_t *pending, const uint8_t *bytes,
                          size_t count, bool armed);

#endif
