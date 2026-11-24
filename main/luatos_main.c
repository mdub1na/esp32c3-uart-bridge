#include "sdkconfig.h"

#ifdef CONFIG_BRIDGE_LUATOS_RECOVERY
#include "d9w_recovery.h"
#include "esp_timer.h"
#else
#include "luatos_frame.h"
#endif

#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hal/gpio_ll.h"

#define TARGET_UART UART_NUM_1
#define TARGET_RX_GPIO GPIO_NUM_3
#define TARGET_TX_GPIO GPIO_NUM_1
#define HOST_UART UART_NUM_0
#define HOST_TX_GPIO GPIO_NUM_21
#define HOST_RX_GPIO GPIO_NUM_20
#define UART_BAUD_RATE 115200
#define TARGET_RX_BUFFER_SIZE 8192
#define HOST_RX_BUFFER_SIZE 512

static void write_host(const void *data, size_t size)
{
    const int sent = uart_write_bytes(HOST_UART, data, size);
    ESP_ERROR_CHECK(sent == (int)size ? ESP_OK : ESP_FAIL);
}

#ifdef CONFIG_BRIDGE_LUATOS_RECOVERY
#define HOST_MESSAGE(message) write_host(message "\r\n", sizeof(message "\r\n") - 1)
#endif

static void configure_target_tx(gpio_mode_t mode)
{
    const gpio_config_t config = {
        .pin_bit_mask = UINT64_C(1) << TARGET_TX_GPIO,
        .mode = mode,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&config));
}

static void release_target_tx(void)
{
    /* Disconnect output before clearing open drain or changing the matrix. */
    ESP_ERROR_CHECK(gpio_set_direction(TARGET_TX_GPIO, GPIO_MODE_INPUT));
    gpio_ll_matrix_out_default(GPIO_LL_GET_HW(0), TARGET_TX_GPIO);
}

static void configure_uarts(void)
{
    const uart_config_t config = {
        .baud_rate = UART_BAUD_RATE,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    ESP_ERROR_CHECK(uart_param_config(HOST_UART, &config));
    ESP_ERROR_CHECK(uart_set_pin(HOST_UART, HOST_TX_GPIO, HOST_RX_GPIO,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    ESP_ERROR_CHECK(uart_driver_install(HOST_UART, HOST_RX_BUFFER_SIZE,
                                        0, 0, NULL, 0));
#ifdef CONFIG_BRIDGE_LUATOS_RECOVERY
    HOST_MESSAGE("D9W-BRIDGE HOST-READY");
#endif

    ESP_ERROR_CHECK(uart_param_config(TARGET_UART, &config));
    ESP_ERROR_CHECK(uart_set_pin(TARGET_UART, UART_PIN_NO_CHANGE, TARGET_RX_GPIO,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    ESP_ERROR_CHECK(uart_driver_install(TARGET_UART, TARGET_RX_BUFFER_SIZE,
                                        0, 0, NULL, 0));
}

enum tx_result { TX_SENT, TX_LOW, TX_ERROR };

static enum tx_result send_line(const void *line, size_t size)
{
    /* The attached line supplies idle high with this pad's pulls disabled. */
    for (int sample = 0; sample < 5; sample++) {
        if (!gpio_get_level(TARGET_TX_GPIO)) {
            return TX_LOW;
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }

    ESP_ERROR_CHECK(gpio_set_level(TARGET_TX_GPIO, 1));
    /* In the pinned IDF, gpio_config applies open drain before output enable. */
    configure_target_tx(GPIO_MODE_INPUT_OUTPUT_OD);
    enum tx_result result = TX_ERROR;
    if (uart_set_pin(TARGET_UART, TARGET_TX_GPIO, UART_PIN_NO_CHANGE,
                     UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE) == ESP_OK) {
        const int sent = uart_write_bytes(TARGET_UART, line, size);
        const esp_err_t done = uart_wait_tx_done(TARGET_UART, pdMS_TO_TICKS(25));
        if (sent == (int)size && done == ESP_OK)
            result = TX_SENT;
    }
    release_target_tx();
    return result;
}

#ifdef CONFIG_BRIDGE_LUATOS_RECOVERY
static uint64_t milliseconds(void)
{
    return (uint64_t)esp_timer_get_time() / 1000;
}

static void send_recovery(void)
{
    static const char command[] = "/system/bin/reboot recovery\n";
    switch (send_line(command, sizeof(command) - 1)) {
    case TX_SENT:
        HOST_MESSAGE("D9W-BRIDGE RECOVERY-SENT");
        break;
    case TX_LOW:
        HOST_MESSAGE("D9W-BRIDGE RX-NOT-IDLE-HIGH");
        break;
    case TX_ERROR:
        HOST_MESSAGE("D9W-BRIDGE RECOVERY-TX-ERROR");
        break;
    }
}

static void poll_host(struct d9w_recovery *bridge)
{
    uint8_t received[128];
    const int count = uart_read_bytes(HOST_UART, received, sizeof(received), 0);
    ESP_ERROR_CHECK(count >= 0 ? ESP_OK : ESP_FAIL);
    for (int i = 0; i < count; i++) {
        size_t size = 0;
        const enum d9w_recovery_tx request =
            d9w_recovery_receive_host(bridge, received[i], milliseconds(), &size);
        if (request == D9W_RECOVERY_TX_REBOOT) {
            send_recovery();
        } else if (request == D9W_RECOVERY_TX_QUERY) {
            /* Target frames remain raw, including replies larger than our line parser. */
            (void)send_line(bridge->host.bytes, size);
        }
    }
}
#else
static void poll_host(luatos_frame_t *frame)
{
    uint8_t received[128];
    const int count = uart_read_bytes(HOST_UART, received, sizeof(received), 0);
    for (int index = 0; index < count; index++) {
        if (luatos_frame_feed(frame, received[index])) {
            (void)send_line(frame->line, frame->line_size);
        }
    }
}
#endif

void app_main(void)
{
#ifdef CONFIG_BRIDGE_LUATOS_RECOVERY
    const gpio_config_t unused = {
        .pin_bit_mask = UINT64_C(1) << GPIO_NUM_0,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&unused));
#endif
    release_target_tx();
    configure_target_tx(GPIO_MODE_INPUT);

    const gpio_config_t target_rx = {
        .pin_bit_mask = UINT64_C(1) << TARGET_RX_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&target_rx));
    configure_uarts();

#ifdef CONFIG_BRIDGE_LUATOS_RECOVERY
    HOST_MESSAGE("D9W-BRIDGE IO03-READY");
    HOST_MESSAGE("D9W-BRIDGE ONE-SHOT-RECOVERY-READY");
    HOST_MESSAGE("D9W-BRIDGE SESSION-READY");
    struct d9w_recovery bridge = {0};
#else
    luatos_frame_t frame = {0};
#endif
    uint8_t bytes[256];
    for (;;) {
#ifndef CONFIG_BRIDGE_LUATOS_RECOVERY
        poll_host(&frame);
#endif
        const int received = uart_read_bytes(TARGET_UART, bytes, sizeof(bytes),
                                             pdMS_TO_TICKS(10));
        ESP_ERROR_CHECK(received >= 0 ? ESP_OK : ESP_FAIL);
        if (received > 0) {
#ifdef CONFIG_BRIDGE_LUATOS_RECOVERY
            const uint64_t now = milliseconds();
            for (int i = 0; i < received; i++)
                d9w_recovery_receive_target(&bridge, bytes[i], now);
#endif
            write_host(bytes, (size_t)received);
        }
#ifdef CONFIG_BRIDGE_LUATOS_RECOVERY
        poll_host(&bridge);
#endif
    }
}
