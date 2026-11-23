#include "luatos_frame.h"

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

    ESP_ERROR_CHECK(uart_param_config(TARGET_UART, &config));
    ESP_ERROR_CHECK(uart_set_pin(TARGET_UART, UART_PIN_NO_CHANGE, TARGET_RX_GPIO,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    ESP_ERROR_CHECK(uart_driver_install(TARGET_UART, TARGET_RX_BUFFER_SIZE,
                                        0, 0, NULL, 0));
}

static void send_line(const uint8_t *line, size_t size)
{
    /* The attached line supplies idle high with this pad's pulls disabled. */
    for (int sample = 0; sample < 5; sample++) {
        if (!gpio_get_level(TARGET_TX_GPIO)) {
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }

    ESP_ERROR_CHECK(gpio_set_level(TARGET_TX_GPIO, 1));
    /* In the pinned IDF, gpio_config applies open drain before output enable. */
    configure_target_tx(GPIO_MODE_INPUT_OUTPUT_OD);
    if (uart_set_pin(TARGET_UART, TARGET_TX_GPIO, UART_PIN_NO_CHANGE,
                     UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE) == ESP_OK) {
        (void)uart_write_bytes(TARGET_UART, line, size);
        (void)uart_wait_tx_done(TARGET_UART, pdMS_TO_TICKS(25));
    }
    release_target_tx();
}

static void poll_host(luatos_frame_t *frame)
{
    uint8_t received[128];
    const int count = uart_read_bytes(HOST_UART, received, sizeof(received), 0);
    for (int index = 0; index < count; index++) {
        if (luatos_frame_feed(frame, received[index])) {
            send_line(frame->line, frame->line_size);
        }
    }
}

void app_main(void)
{
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

    luatos_frame_t frame = {0};
    uint8_t bytes[256];
    for (;;) {
        poll_host(&frame);
        const int received = uart_read_bytes(TARGET_UART, bytes, sizeof(bytes),
                                             pdMS_TO_TICKS(10));
        ESP_ERROR_CHECK(received >= 0 ? ESP_OK : ESP_FAIL);
        if (received > 0) {
            const int sent = uart_write_bytes(HOST_UART, bytes, (size_t)received);
            ESP_ERROR_CHECK(sent == received ? ESP_OK : ESP_FAIL);
        }
    }
}
