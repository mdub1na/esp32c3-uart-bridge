#include "bridge_control.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void release_button(bridge_control_t *control, uint64_t time)
{
    bridge_control_update(control, time, false, true, false);
    bridge_control_update(control, time + BRIDGE_RELEASE_DEBOUNCE_MS,
                           false, true, false);
}

static void hold_to_arm(bridge_control_t *control, uint64_t time)
{
    bridge_control_update(control, time, true, true, false);
    bridge_control_update(control, time + BRIDGE_ARM_HOLD_MS - 1,
                           true, true, false);
    assert(control->mode == BRIDGE_RECEIVE_ONLY);
    bridge_control_update(control, time + BRIDGE_ARM_HOLD_MS,
                           true, true, false);
    assert(control->mode == BRIDGE_ARMED);
}

static void test_arm_and_disarm(void)
{
    bridge_control_t control;
    bridge_control_init(&control, false);
    hold_to_arm(&control, 100);
    /* The arming hold itself keeps the mode armed. */
    bridge_control_update(&control, 5000, true, true, false);
    assert(control.mode == BRIDGE_ARMED);
    release_button(&control, 5010);
    bridge_control_update(&control, 5100, true, true, false);
    assert(control.mode == BRIDGE_RECEIVE_ONLY);
    bridge_control_update(&control, 10000, true, true, false);
    assert(control.mode == BRIDGE_RECEIVE_ONLY);
    /* A brief release bounce does not permit a second long press. */
    bridge_control_update(&control, 10001, false, true, false);
    bridge_control_update(&control, 10002, true, true, false);
    bridge_control_update(&control, 12000, true, true, false);
    assert(control.mode == BRIDGE_RECEIVE_ONLY);
    release_button(&control, 12010);
    hold_to_arm(&control, 12100);
}

static void test_short_press_and_reset_hold(void)
{
    bridge_control_t control;
    bridge_control_init(&control, false);
    bridge_control_update(&control, 10, true, true, false);
    release_button(&control, 100);
    bridge_control_update(&control, 5000, false, true, false);
    assert(control.mode == BRIDGE_RECEIVE_ONLY);
    bridge_control_init(&control, true);
    bridge_control_update(&control, 10000, true, true, false);
    assert(control.mode == BRIDGE_RECEIVE_ONLY);
    release_button(&control, 10010);
    hold_to_arm(&control, 10100);
}

static void test_disconnect_and_fault(void)
{
    bridge_control_t control;
    bridge_control_init(&control, false);
    hold_to_arm(&control, 0);
    bridge_control_update(&control, 2000, true, false, false);
    assert(control.mode == BRIDGE_RECEIVE_ONLY);
    bridge_control_update(&control, 10000, true, true, false);
    assert(control.mode == BRIDGE_RECEIVE_ONLY);
    bridge_control_update(&control, 10001, false, false, false);
    bridge_control_update(&control, 10002, true, true, false);
    bridge_control_update(&control, 15000, true, true, false);
    assert(control.mode == BRIDGE_RECEIVE_ONLY);
    release_button(&control, 15010);
    hold_to_arm(&control, 15100);
    bridge_control_update(&control, 17000, false, true, true);
    assert(control.mode == BRIDGE_FAULT);
    release_button(&control, 17010);
    bridge_control_update(&control, 20000, true, true, false);
    bridge_control_test_arm(&control, true);
    assert(control.mode == BRIDGE_FAULT);
}

static void test_disconnect_during_hold(void)
{
    bridge_control_t control;
    bridge_control_init(&control, false);
    bridge_control_update(&control, 0, true, true, false);
    bridge_control_update(&control, 1000, true, false, false);
    bridge_control_update(&control, 10000, true, true, false);
    assert(control.mode == BRIDGE_RECEIVE_ONLY);
    release_button(&control, 10010);
    hold_to_arm(&control, 10100);
}

static void test_pending_partial_and_discard(void)
{
    bridge_pending_t pending = {0};
    uint8_t pattern[BRIDGE_PENDING_CAPACITY];
    for (size_t i = 0; i < sizeof(pattern); ++i) {
        pattern[i] = (uint8_t)i;
    }
    assert(bridge_pending_stage(&pending, pattern, sizeof(pattern), false));
    assert(bridge_pending_remaining(&pending) == 0);
    assert(bridge_pending_stage(&pending, pattern, sizeof(pattern), true));
    assert(memcmp(pending.bytes, pattern, sizeof(pattern)) == 0);
    assert(bridge_pending_consume(&pending, 17));
    assert(bridge_pending_remaining(&pending) == sizeof(pattern) - 17);
    assert(!bridge_pending_consume(&pending, sizeof(pattern)));
    assert(!bridge_pending_stage(&pending, pattern, 1, true));
    /* Disarm abandons the unsent suffix; a new arm starts empty. */
    bridge_pending_clear(&pending);
    assert(bridge_pending_remaining(&pending) == 0);
    assert(bridge_pending_stage(&pending, pattern, 3, true));
    assert(bridge_pending_consume(&pending, 1));
    assert(bridge_pending_consume(&pending, 2));
    assert(pending.offset == 0 && pending.length == 0);
    assert(!bridge_pending_stage(&pending, pattern, sizeof(pattern) + 1, true));
}

static void test_test_mode_arm(void)
{
    bridge_control_t control;
    bridge_control_init(&control, false);
    bridge_control_test_arm(&control, false);
    assert(control.mode == BRIDGE_RECEIVE_ONLY);
    bridge_control_test_arm(&control, true);
    assert(control.mode == BRIDGE_ARMED);
    bridge_control_init(&control, true);
    bridge_control_test_arm(&control, true);
    assert(control.mode == BRIDGE_RECEIVE_ONLY);
}

int main(void)
{
    test_arm_and_disarm();
    test_short_press_and_reset_hold();
    test_disconnect_and_fault();
    test_disconnect_during_hold();
    test_pending_partial_and_discard();
    test_test_mode_arm();
    puts("bridge control tests passed");
    return 0;
}
