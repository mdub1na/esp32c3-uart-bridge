#include "bridge_control.h"

#include <string.h>

void bridge_control_init(bridge_control_t *control, bool button_pressed)
{
    *control = (bridge_control_t){
        .mode = BRIDGE_RECEIVE_ONLY,
        .press_active = button_pressed,
        .release_confirmed = !button_pressed,
    };
}

void bridge_control_update(bridge_control_t *control, uint64_t now_ms,
                           bool button_pressed, bool usb_connected, bool fault)
{
    if (fault || control->mode == BRIDGE_FAULT) {
        control->mode = BRIDGE_FAULT;
        control->hold_candidate = false;
        return;
    }
    if (!usb_connected) {
        control->mode = BRIDGE_RECEIVE_ONLY;
        control->hold_candidate = false;
    }

    if (button_pressed) {
        control->release_pending = false;
        if (!control->press_active) {
            control->press_active = true;
            control->hold_candidate = usb_connected && control->release_confirmed &&
                                      control->mode == BRIDGE_RECEIVE_ONLY;
            control->release_confirmed = false;
            control->pressed_at_ms = now_ms;
            if (control->mode == BRIDGE_ARMED) {
                /* First pressed sample disarms; this same hold cannot arm. */
                control->mode = BRIDGE_RECEIVE_ONLY;
                control->hold_candidate = false;
            }
        }
        if (control->hold_candidate &&
            now_ms - control->pressed_at_ms >= BRIDGE_ARM_HOLD_MS) {
            control->mode = BRIDGE_ARMED;
            control->hold_candidate = false;
        }
    } else if (control->press_active) {
        if (!control->release_pending) {
            control->release_pending = true;
            control->released_at_ms = now_ms;
        }
        if (now_ms - control->released_at_ms >= BRIDGE_RELEASE_DEBOUNCE_MS) {
            control->press_active = false;
            control->release_confirmed = true;
            control->hold_candidate = false;
            control->release_pending = false;
        }
    } else {
        control->release_confirmed = true;
    }
}

void bridge_control_test_arm(bridge_control_t *control, bool usb_connected)
{
    if (usb_connected && control->mode == BRIDGE_RECEIVE_ONLY &&
        !control->press_active) {
        control->mode = BRIDGE_ARMED;
    }
}

void bridge_pending_clear(bridge_pending_t *pending)
{
    pending->length = 0;
    pending->offset = 0;
}

size_t bridge_pending_remaining(const bridge_pending_t *pending)
{
    return pending->length - pending->offset;
}

bool bridge_pending_consume(bridge_pending_t *pending, size_t count)
{
    if (count > bridge_pending_remaining(pending)) {
        return false;
    }
    pending->offset += count;
    if (pending->offset == pending->length) {
        bridge_pending_clear(pending);
    }
    return true;
}

bool bridge_pending_stage(bridge_pending_t *pending, const uint8_t *bytes,
                          size_t count, bool armed)
{
    if (!armed) {
        bridge_pending_clear(pending);
        return true;
    }
    if (count > sizeof(pending->bytes) || bridge_pending_remaining(pending) != 0) {
        return false;
    }
    if (count != 0) {
        memcpy(pending->bytes, bytes, count);
    }
    pending->length = count;
    pending->offset = 0;
    return true;
}
