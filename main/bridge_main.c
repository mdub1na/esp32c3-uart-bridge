#include "bridge_control.h"

#include "driver/gpio.h"
#include "driver/uart.h"
#include "driver/usb_serial_jtag.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "hal/gpio_ll.h"
#include "hal/uart_ll.h"
#include "hal/usb_serial_jtag_ll.h"
#include "sdkconfig.h"

#define BRIDGE_UART UART_NUM_1
#define BRIDGE_TX_GPIO GPIO_NUM_1
#define BRIDGE_RX_GPIO GPIO_NUM_3
#define BRIDGE_LED_GPIO GPIO_NUM_8
#define BRIDGE_BOOT_GPIO GPIO_NUM_9
#define BRIDGE_UART_RX_SIZE 8192
#define BRIDGE_USB_BUFFER_SIZE 4096
#define BRIDGE_UART_EVENT_COUNT 32
#define BRIDGE_USB_DISCARD_BUDGET (2 * BRIDGE_USB_BUFFER_SIZE)

static QueueHandle_t s_uart_events;
static bridge_control_t s_control;
static bridge_pending_t s_to_uart;
static bridge_pending_t s_to_usb;
static bool s_uart_installed;
static bool s_usb_installed;
static portMUX_TYPE s_usb_rx_gate = portMUX_INITIALIZER_UNLOCKED;

static uint64_t now_ms(void)
{
    return (uint64_t)esp_timer_get_time() / UINT64_C(1000);
}

static esp_err_t tx_pin_receive_only(void)
{
    /* Disable the pad BEFORE changing its routing or touching queued bytes.
     * gpio_reset_pin alone enables a pull-up and retains the matrix route.
     * The pinned ESP-IDF LL operation disconnects without enabling the output,
     * unlike esp_rom_gpio_connect_out_signal, which enables the pad output.
     */
    esp_err_t status = gpio_set_direction(BRIDGE_TX_GPIO, GPIO_MODE_INPUT);
    gpio_ll_matrix_out_default(GPIO_LL_GET_HW(0), BRIDGE_TX_GPIO);
    const esp_err_t pull_up = gpio_pullup_dis(BRIDGE_TX_GPIO);
    const esp_err_t pull_down = gpio_pulldown_dis(BRIDGE_TX_GPIO);
    if (status != ESP_OK) {
        return status;
    }
    return pull_up != ESP_OK ? pull_up : pull_down;
}

static void discard_uart_transmit(void)
{
    bridge_pending_clear(&s_to_uart);
    if (s_uart_installed) {
        /* There is no UART software TX buffer, no other writer and no TX ISR.
         * Reset only TX FIFO; waiting is never needed before isolating GPIO1.
         * Re-arming also waits for any in-progress shift register byte to end.
         */
        uart_ll_txfifo_rst(UART_LL_GET_HW(BRIDGE_UART));
    }
}

static esp_err_t discard_usb_input(void)
{
    uint8_t discarded[BRIDGE_PENDING_CAPACITY];
    if (!s_usb_installed) {
        return ESP_OK;
    }
    /* Mask only OUT reception while draining. Otherwise an unserviced hardware
     * OUT packet could predate arming yet enter the driver's ring afterward.
     * This task and the driver's ISR share one ESP32-C3 core. Short critical
     * sections protect interrupt-mask read/modify/write against the TX ISR.
     * The USB TX interrupt and SOF connection monitor stay enabled.
     */
    portENTER_CRITICAL(&s_usb_rx_gate);
    usb_serial_jtag_ll_disable_intr_mask(USB_SERIAL_JTAG_INTR_SERIAL_OUT_RECV_PKT);
    portEXIT_CRITICAL(&s_usb_rx_gate);
    esp_err_t result = ESP_ERR_TIMEOUT;
    for (size_t total = 0; total < BRIDGE_USB_DISCARD_BUDGET;) {
        const int count = usb_serial_jtag_read_bytes(discarded, sizeof(discarded), 0);
        if (count < 0 || count > (int)sizeof(discarded)) {
            result = ESP_FAIL;
            break;
        }
        if (count == 0) {
            result = ESP_OK;
            break;
        }
        total += (size_t)count;
    }
    if (result == ESP_OK) {
        /* Match the pinned driver's acknowledge-before-read ordering. The
         * hardware drain is bounded too; continuous traffic cannot spin here.
         */
        usb_serial_jtag_ll_clr_intsts_mask(USB_SERIAL_JTAG_INTR_SERIAL_OUT_RECV_PKT);
        (void)usb_serial_jtag_ll_read_rxfifo(discarded, sizeof(discarded));
        if (usb_serial_jtag_ll_rxfifo_data_available()) {
            result = ESP_ERR_TIMEOUT;
        }
    }
    portENTER_CRITICAL(&s_usb_rx_gate);
    usb_serial_jtag_ll_ena_intr_mask(USB_SERIAL_JTAG_INTR_SERIAL_OUT_RECV_PKT);
    portEXIT_CRITICAL(&s_usb_rx_gate);
    return result;
}

static void latch_fault(void)
{
    (void)tx_pin_receive_only();
    discard_uart_transmit();
    bridge_pending_clear(&s_to_usb);
    bridge_control_update(&s_control, now_ms(), false, false, true);
}

static bool configure_gpio(void)
{
    if (tx_pin_receive_only() != ESP_OK) {
        return false;
    }
    const gpio_config_t input = {
        .pin_bit_mask = (UINT64_C(1) << BRIDGE_RX_GPIO),
        .mode = GPIO_MODE_INPUT,
        /* Keep the UART idle-high when no external wire is attached. This
         * weak pull-up is to 3.3 V; a 1.8 V UART always needs a level shifter.
         * The separately isolated TX pad still has both pulls disabled.
         */
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    const gpio_config_t button = {
        .pin_bit_mask = (UINT64_C(1) << BRIDGE_BOOT_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    /* Preload inactive LED level before enabling its active-low output. */
    return gpio_config(&input) == ESP_OK && gpio_config(&button) == ESP_OK &&
           gpio_set_level(BRIDGE_LED_GPIO, 1) == ESP_OK &&
           gpio_set_direction(BRIDGE_LED_GPIO, GPIO_MODE_OUTPUT) == ESP_OK;
}

static bool configure_uart(void)
{
    const uart_config_t config = {
        .baud_rate = 115200,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    if (uart_param_config(BRIDGE_UART, &config) != ESP_OK ||
        uart_driver_install(BRIDGE_UART, BRIDGE_UART_RX_SIZE, 0,
                            BRIDGE_UART_EVENT_COUNT, &s_uart_events, 0) != ESP_OK) {
        return false;
    }
    s_uart_installed = true;
    /* UART_PIN_NO_CHANGE is not a disconnect operation. GPIO1 was explicitly
     * isolated above; only the RX input is connected at initialization.
     */
    if (uart_set_pin(BRIDGE_UART, UART_PIN_NO_CHANGE, BRIDGE_RX_GPIO,
                     UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE) != ESP_OK ||
        uart_set_rx_timeout(BRIDGE_UART, 2) != ESP_OK) {
        return false;
    }
#if CONFIG_BRIDGE_INTERNAL_LOOPBACK
    if (uart_set_loop_back(BRIDGE_UART, true) != ESP_OK) {
        return false;
    }
    /* Before loopback takes over, the disconnected RX pad can produce an
     * initial character or error. Let that in-flight character finish, then
     * discard only this diagnostic startup residue before automatic arming.
     * Normal builds preserve external UART bytes received during startup.
     */
    vTaskDelay(pdMS_TO_TICKS(2));
    if (uart_flush_input(BRIDGE_UART) != ESP_OK ||
        xQueueReset(s_uart_events) != pdPASS) {
        return false;
    }
#endif
    return true;
}

static bool configure_usb(void)
{
    usb_serial_jtag_driver_config_t config = {
        .tx_buffer_size = BRIDGE_USB_BUFFER_SIZE,
        .rx_buffer_size = BRIDGE_USB_BUFFER_SIZE,
    };
    if (usb_serial_jtag_driver_install(&config) != ESP_OK) {
        return false;
    }
    s_usb_installed = true;
    return true;
}

static bool uart_has_error(void)
{
    uart_event_t event;
    for (unsigned int i = 0; i < BRIDGE_UART_EVENT_COUNT; ++i) {
        if (xQueueReceive(s_uart_events, &event, 0) != pdTRUE) {
            return false;
        }
        switch (event.type) {
        case UART_FIFO_OVF:
        case UART_BUFFER_FULL:
        case UART_BREAK:
        case UART_PARITY_ERR:
        case UART_FRAME_ERR:
            return true;
        default:
            break;
        }
    }
    return false;
}

static bool apply_mode_change(bridge_mode_t previous)
{
    if (s_control.mode == previous) {
        return true;
    }
    if (tx_pin_receive_only() != ESP_OK) {
        return false;
    }
    discard_uart_transmit();
    if (discard_usb_input() != ESP_OK) {
        return false;
    }
    if (s_control.mode == BRIDGE_ARMED) {
        if (uart_wait_tx_done(BRIDGE_UART, pdMS_TO_TICKS(2)) != ESP_OK) {
            return false;
        }
#if !CONFIG_BRIDGE_INTERNAL_LOOPBACK
        if (uart_set_pin(BRIDGE_UART, BRIDGE_TX_GPIO, UART_PIN_NO_CHANGE,
                         UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE) != ESP_OK) {
            return false;
        }
#endif
    }
    return true;
}

static bool forward_host_bytes(void)
{
    if (s_control.mode != BRIDGE_ARMED) {
        bridge_pending_clear(&s_to_uart);
        return discard_usb_input() == ESP_OK;
    }
    if (bridge_pending_remaining(&s_to_uart) == 0) {
        uint8_t bytes[BRIDGE_PENDING_CAPACITY];
        const int count = usb_serial_jtag_read_bytes(bytes, sizeof(bytes), 0);
        if (count < 0 || count > (int)sizeof(bytes) ||
            !bridge_pending_stage(&s_to_uart, bytes, (size_t)count,
                                   s_control.mode == BRIDGE_ARMED)) {
            return false;
        }
    }
    const size_t remaining = bridge_pending_remaining(&s_to_uart);
    if (remaining != 0) {
        const int sent = uart_tx_chars(BRIDGE_UART,
                                       (const char *)s_to_uart.bytes + s_to_uart.offset,
                                       (uint32_t)remaining);
        if (sent < 0 || !bridge_pending_consume(&s_to_uart, (size_t)sent)) {
            return false;
        }
    }
    return true;
}

static bool forward_uart_bytes(bool connected)
{
    if (!connected) {
        bridge_pending_clear(&s_to_usb);
        return uart_flush_input(BRIDGE_UART) == ESP_OK;
    }
    if (bridge_pending_remaining(&s_to_usb) == 0) {
        const int count = uart_read_bytes(BRIDGE_UART, s_to_usb.bytes,
                                          sizeof(s_to_usb.bytes), 0);
        if (count < 0 || count > (int)sizeof(s_to_usb.bytes)) {
            return false;
        }
        s_to_usb.length = (size_t)count;
        s_to_usb.offset = 0;
    }
    const size_t remaining = bridge_pending_remaining(&s_to_usb);
    if (remaining != 0) {
        const int sent = usb_serial_jtag_write_bytes(s_to_usb.bytes + s_to_usb.offset,
                                                     remaining, 0);
        /* ESP-IDF 5.5.2 enqueues all or none; retain offsets defensively. A full
         * USB TX buffer is ordinary backpressure, not a reason to spin/wait.
         */
        if (sent < 0 || !bridge_pending_consume(&s_to_usb, (size_t)sent)) {
            return false;
        }
    }
    return true;
}

static void update_led(uint64_t time_ms)
{
    bool on;
    switch (s_control.mode) {
    case BRIDGE_ARMED:
        on = true;
        break;
    case BRIDGE_FAULT:
        on = (time_ms / 100U) % 2U == 0;
        break;
    default:
        on = (time_ms / 500U) % 2U == 0;
        break;
    }
    if (gpio_set_level(BRIDGE_LED_GPIO, on ? 0 : 1) != ESP_OK) {
        latch_fault();
    }
}

void app_main(void)
{
    bridge_control_init(&s_control, true);
    const bool ready = configure_gpio() && configure_uart() && configure_usb();
    bridge_control_init(&s_control, gpio_get_level(BRIDGE_BOOT_GPIO) == 0);
    if (!ready) {
        latch_fault();
    }
#if CONFIG_BRIDGE_INTERNAL_LOOPBACK
    bool test_arm_pending = true;
#endif
    for (;;) {
        const uint64_t time_ms = now_ms();
        /* SOF presence detects an attached active USB host. Closing the host
         * serial file does not disconnect USB and does not disarm this bridge.
         */
        const bool connected = usb_serial_jtag_is_connected();
        const bridge_mode_t previous = s_control.mode;
        const bool error = s_uart_installed && uart_has_error();
        bridge_control_update(&s_control, time_ms,
                               gpio_get_level(BRIDGE_BOOT_GPIO) == 0,
                               connected, error);
#if CONFIG_BRIDGE_INTERNAL_LOOPBACK
        if (test_arm_pending && connected) {
            bridge_control_test_arm(&s_control, connected);
            test_arm_pending = false;
        }
#endif
        if (!apply_mode_change(previous)) {
            latch_fault();
        }
        if (s_control.mode != BRIDGE_FAULT) {
            if (!forward_host_bytes() || !forward_uart_bytes(connected)) {
                latch_fault();
            }
        } else if (s_usb_installed) {
            (void)discard_usb_input();
        }
        update_led(time_ms);
        /* One owner performs all I/O; all data-path driver calls have timeout
         * zero. A full FIFO or USB buffer always yields to the next tick.
         */
        vTaskDelay(1);
    }
}
