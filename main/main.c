#include <stdio.h>
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "TCA9554PWR.h"
#include "ST7701S.h"
#include "GT911.h"
#include "LVGL_Driver.h"
#include "boot/boot_screen.h"
#include "ui/screens/ui_Screen1.h"
#include "ui/ui.h"
#include "driver/uart.h"
#include "esp_log.h"
#include <string.h>
#include <stdint.h>
#include <math.h>

#define UART_PORT      UART_NUM_1
#define UART_RX_PIN    44
#define UART_BAUD      2000000

#define ENABLE_LOGGING 0

#define SOF_BYTE       0xA5
#define PKT_LEN        26

static const char *TAG = "RX";

typedef struct __attribute__((packed)) {
    uint16_t oil_temp;      
    uint16_t water_temp;    
    uint16_t oil_pressure;  
    uint16_t fuel_pressure; 
    uint16_t fuel_level;   
    uint16_t afr;         
    int16_t boost;          
    uint32_t lap_time_ms;    
    int32_t  lap_delta_ms; 
} gauge_payload_t;

static const float BASE_FUEL_PRESSURE = 43.0;

static int g_water_temp = 200;
static int g_oil_temp = 142;
static int g_oil_pressure = 2;
static int g_fuel_pressure = 39;
static int g_fuel_level = 10.0;
static int g_boost = 0.0;

static uint16_t crc16_ccitt(const uint8_t *data, uint16_t len){
    uint16_t crc = 0xFFFF;
    for (int i = 0; i < len; i++) {
        crc ^= (uint16_t)data[i] << 8;
        for (int j = 0; j < 8; j++)
            crc = (crc & 0x8000) ? (crc << 1) ^ 0x1021 : (crc << 1);
    }
    return crc;
}




static void uart_init(void)
{
    uart_config_t cfg = {
        .baud_rate = UART_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT
    };

    ESP_ERROR_CHECK(uart_param_config(UART_PORT, &cfg));

    ESP_ERROR_CHECK(uart_set_pin(
        UART_PORT,
        UART_PIN_NO_CHANGE,
        UART_RX_PIN,
        UART_PIN_NO_CHANGE,
        UART_PIN_NO_CHANGE
    ));

    ESP_ERROR_CHECK(uart_driver_install(
        UART_PORT,
        4096,   // RX buffer (bigger)
        0,
        0,
        NULL,
        0
    ));
}


void update_gauge_label(lv_obj_t *label, float value)
{
    char buf[8];
    snprintf(buf, sizeof(buf), "%.2f", value);
    lv_label_set_text(label, buf);
}

static void uart_rx_task(void *arg){
    uint8_t buf[PKT_LEN];
    int idx = 0;

    while (1) {
        uint8_t byte;
        if (uart_read_bytes(UART_PORT, &byte, 1, portMAX_DELAY) != 1)
            continue;

        // sync on SOF
        if (idx == 0 && byte != SOF_BYTE)
            continue;

        buf[idx++] = byte;

        if (idx == PKT_LEN) {
            idx = 0;

            uint16_t rx_crc = buf[PKT_LEN - 2] | (buf[PKT_LEN - 1] << 8);

            uint16_t calc_crc = crc16_ccitt(&buf[1], PKT_LEN - 3);

            if (rx_crc != calc_crc) {
                ESP_LOGW(TAG, "CRC fail");
                continue;
            }

            gauge_payload_t p;
            memcpy(&p, &buf[2], sizeof(p));

            float afr = p.afr * 0.1f; 
            float boost = p.boost * 0.1f;
            float water_temp = p.water_temp *0.1f;
            float oil_temp = p.oil_temp *0.1f;
            float oil_pressure = p.oil_pressure *0.1f;
            float fuel_pressure = p.fuel_pressure *0.1f;
            float fuel_level = p.fuel_level *0.1f;

            g_water_temp = (int)water_temp;
            g_oil_temp = (int)oil_temp;
            g_oil_pressure = (int)oil_pressure;
            g_fuel_pressure = (int)fuel_pressure;
            g_fuel_level = (int)fuel_level;
            g_boost = boost;

            #if ENABLE_LOGGING
            ESP_LOGI(TAG,
                "Oil %.1fC Water %.1fC AFR %.2f Boost %.1fkPa",
                p->oil_temp / 10.0f,
                p->water_temp / 10.0f,
                p->afr / 10.0f,
                p->boost / 10.0f
            );
            #endif
        }
    }
}




bool isFuelPressureOK() {
    return fabs(g_fuel_pressure - (BASE_FUEL_PRESSURE + g_boost)) <= 4.0;
}

// Push the latest UART values into the gauges. Colors, warnings and the
// low-fuel lamp are handled by the screen (see ui/screens/ui_Screen1.h).
void gauge_timer(lv_timer_t * t) {
    ui_left_set_coolant(g_water_temp);
    ui_left_set_oil_pressure(g_oil_pressure);
    ui_left_set_oil_temp(g_oil_temp);
}

// Fuel level sloshes around, so it's only refreshed once a second
void fuel_timer(lv_timer_t * t){
    ui_left_set_fuel(g_fuel_level);
}


void app_main(void){   
    I2C_Init();
    EXIO_Init();
    LCD_Init();
    Touch_Init();
    LVGL_Init();

    // LVGL isn't thread-safe: hold the lock while building the UI so the
    // LVGL task can't render at the same time (that crashes at boot).
    LVGL_Lock(-1);
    ui_init();
    lv_timer_create(gauge_timer, 100, NULL);
    lv_timer_create(fuel_timer, 1000, NULL);
    ui_left_set_fuel(g_fuel_level);
    LVGL_Unlock();

    uart_init();

    xTaskCreate(
        uart_rx_task,
        "uart_rx",
        4096,
        NULL,
        10,
        NULL
    );

    Set_Backlight(0); 
    vTaskDelay(pdMS_TO_TICKS(750)); 
    Set_Backlight(100);
}