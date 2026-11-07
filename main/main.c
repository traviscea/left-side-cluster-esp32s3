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

#include "esp_wifi.h"
#include "esp_now.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"

#include "soc/rtc.h"
#include "esp_timer.h"

static lv_color_t green_color;
static lv_color_t red_color;
static lv_color_t orange_color;
static lv_color_t purple_color;
static lv_color_t pink_color;
static lv_color_t blue_color;



typedef struct __attribute__((packed)) {
    uint8_t rotorID;
    int16_t temperature;
} BrakePacket;



static BrakePacket brakePackets[4] =
{
    {0, 70},
    {1, 500},
    {2, 1000},
    {3, 70}
};
static bool espNowRunning = false;
static bool espNowSetup = false;


static lv_timer_t *gaugeTimer = NULL;

static bool low_fuel_blink_state = false;

// =========================================
// BRAKE TEMP SCALE
// =========================================

static float BRAKE_TEMP_MIN = 150.0f;
static float BRAKE_TEMP_MAX = 1000.0f;

static int g_brake_fl = 72;
static int g_brake_fr = 72;
static int g_brake_rl = 72;
static int g_brake_rr = 72;

static bool screen2_initialized = false;

#define UART_PORT      UART_NUM_1
#define UART_RX_PIN    44
#define UART_BAUD      2000000

#define ENABLE_LOGGING 0

#define SOF_BYTE       0xA5
#define PKT_LEN        26

#define BTN_NEXT_SCREEN  7

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

static int g_water_temp = 100;
static int g_oil_temp = 142;
static int g_oil_pressure = 2;
static int g_fuel_pressure = 39;
static int g_fuel_level = 14.0;
static int g_boost = 0.0;

static uint32_t brakeLastSeen[4] = {
    -5000,
    -5000,
    -5000,
    -5000
};

static uint32_t disconnectStart[4] = {
    0,0,0,0
};

static bool brakeConnectedState[4];




static uint16_t crc16_ccitt(const uint8_t *data, uint16_t len){
    uint16_t crc = 0xFFFF;
    for (int i = 0; i < len; i++) {
        crc ^= (uint16_t)data[i] << 8;
        for (int j = 0; j < 8; j++)
            crc = (crc & 0x8000) ? (crc << 1) ^ 0x1021 : (crc << 1);
    }
    return crc;
}


lv_color_t brake_temp_to_color(float temp)
{
    if(temp < BRAKE_TEMP_MIN)
        temp = BRAKE_TEMP_MIN;

    if(temp > BRAKE_TEMP_MAX)
        temp = BRAKE_TEMP_MAX;

    float t =
        (temp - BRAKE_TEMP_MIN)
        /
        (BRAKE_TEMP_MAX - BRAKE_TEMP_MIN);

    float hue =
        240.0f
        *
        (1.0f - t);

    float saturation = 1.0f;
    float value = 1.0f;

    float c =
        value * saturation;

    float x =
        c *
        (
            1.0f
            -
            fabsf(
                fmodf(
                    hue / 60.0f,
                    2.0f
                )
                -
                1.0f
            )
        );

    float m =
        value - c;

    float r = 0;
    float g = 0;
    float b = 0;

    if(hue < 60.0f)
    {
        r = c;
        g = x;
        b = 0;
    }
    else if(hue < 120.0f)
    {
        r = x;
        g = c;
        b = 0;
    }
    else if(hue < 180.0f)
    {
        r = 0;
        g = c;
        b = x;
    }
    else
    {
        r = 0;
        g = x;
        b = c;
    }

    return lv_color_make(
        (uint8_t)((r + m) * 255.0f),
        (uint8_t)((g + m) * 255.0f),
        (uint8_t)((b + m) * 255.0f)
    );
}


void updateRotor(BrakeRotorWidget *brake,float temp)
{
    if(!brake)
        return;

    if(!brake->rotor)
        return;

    if(!brake->label)
        return;

    lv_color_t c =
        brake_temp_to_color(temp);

    lv_obj_set_style_bg_color(
        brake->rotor,
        c,
        0
    );

    char buf[16];

    snprintf(
        buf,
        sizeof(buf),
        "%.0f",
        temp
    );

    lv_label_set_text(
        brake->label,
        buf
    );
}



void on_brake_data(
    const esp_now_recv_info_t *info,
    const uint8_t *data,
    int len
)
{
    if(!espNowRunning)
        return;

    if(len != sizeof(BrakePacket))
        return;

    BrakePacket pkt;

    memcpy(
        &pkt,
        data,
        sizeof(BrakePacket)
    );

    if(pkt.rotorID > 3)
        return;

    brakeLastSeen[pkt.rotorID] =
        lv_tick_get();

    memcpy(
        &brakePackets[pkt.rotorID],
        &pkt,
        sizeof(BrakePacket)
    );
}


void esp_now_setup() {
    // =====================================
    // WIFI INIT ONCE
    // =====================================

    ESP_ERROR_CHECK(
        nvs_flash_init()
    );

    ESP_ERROR_CHECK(
        esp_netif_init()
    );

    ESP_ERROR_CHECK(
        esp_event_loop_create_default()
    );

    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg =
        WIFI_INIT_CONFIG_DEFAULT();

    ESP_ERROR_CHECK(
        esp_wifi_init(&cfg)
    );

    ESP_ERROR_CHECK(
        esp_wifi_set_mode(
            WIFI_MODE_STA
        )
    );

    ESP_ERROR_CHECK(
        esp_wifi_start()
    );

    ESP_ERROR_CHECK(
        esp_wifi_set_max_tx_power(60)
    );

    ESP_ERROR_CHECK(
        esp_wifi_set_ps(WIFI_PS_MIN_MODEM)
    );

    uint8_t mac[6];

    esp_wifi_get_mac(
        WIFI_IF_STA,
        mac
    );

    ESP_LOGI(
        TAG,
        "MAC: %02X:%02X:%02X:%02X:%02X:%02X",
        mac[0],
        mac[1],
        mac[2],
        mac[3],
        mac[4],
        mac[5]
    );

    // =====================================
    // ESPNOW INIT ONCE
    // =====================================

    ESP_ERROR_CHECK(
        esp_now_init()
    );

    esp_now_register_recv_cb(
        on_brake_data
    );

    ESP_LOGI(
        TAG,
        "ESP-NOW READY"
    );
}



void update_brake_screen(void)
{
    static float uiTemps[4] = {0};

    BrakeRotorWidget* brakes[4] = {
        &brake_FL,
        &brake_FR,
        &brake_RL,
        &brake_RR
    };

    uint32_t now =
        lv_tick_get();

    for(int i = 0; i < 4; i++)
    {
        bool connected =
            (now - brakeLastSeen[i]) < 2000;

        if(!connected)
        {
            uiTemps[i] =
                (1000.0f * 0.02f)
                +
                (uiTemps[i] * 0.98f);

            updateRotor(
                brakes[i],
                uiTemps[i]
            );

            continue;
        }

        float target =
            (float)brakePackets[i].temperature;

        // Initialize on first packet
        if(uiTemps[i] <= 0.1f)
        {
            uiTemps[i] = target;
        }

        // EMA smoothing
        uiTemps[i] =
            (target * 0.10f)
            +
            (uiTemps[i] * 0.90f);

        updateRotor(
            brakes[i],
            uiTemps[i]
        );
    }
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

        vTaskDelay(pdMS_TO_TICKS(1));
    }
}




static inline int map_int(int x,
                          int in_min, int in_max,
                          int out_min, int out_max)
{
    return (x - in_min) * (out_max - out_min)
           / (in_max - in_min)
           + out_min;
}

static inline int constrain_int(int x, int low, int high){
    if (x < low) return low;
    if (x > high) return high;
    return x;
}

static void init_label_styles(void){

    blue_color = lv_palette_main(LV_PALETTE_CYAN);
    green_color = lv_color_hex(0x28FF00);
    red_color = lv_palette_main(LV_PALETTE_PINK);
    orange_color = lv_palette_main(LV_PALETTE_DEEP_ORANGE);
    purple_color = lv_palette_main(LV_PALETTE_PURPLE);
    pink_color = lv_palette_main(LV_PALETTE_PINK);
}

static void update_label_if_needed(lv_obj_t *label, int new_value, lv_color_t new_color) {
    if (!label) return;  // safety check

    char buf[12];
    snprintf(buf, sizeof(buf), "%d", new_value);

    // Only update text if changed
    const char *old_text = lv_label_get_text(label);
    if (strcmp(old_text, buf) != 0) {
        lv_label_set_text(label, buf);
    }

    // Only update color if changed
    lv_color_t old_color = lv_obj_get_style_text_color(label, LV_PART_MAIN | LV_STATE_DEFAULT);
    if (old_color.full != new_color.full) {
        lv_obj_set_style_text_color(label, new_color, LV_PART_MAIN | LV_STATE_DEFAULT);
    }
}

bool isFuelPressureOK() {
    return fabs(g_fuel_pressure - (BASE_FUEL_PRESSURE + g_boost)) <= 4.0;
}

void update_gauge_values(){
    lv_color_t water_temp_color = green_color;
    lv_color_t oil_temp_color = green_color;
    lv_color_t oil_pressure_color = green_color;
    lv_color_t fuel_pressure_color = green_color;


    fuel_pressure_color = (!isFuelPressureOK() || g_fuel_pressure < 30) ? red_color : green_color;
    water_temp_color = (g_water_temp < 150) ? blue_color : ((g_water_temp > 205) ? red_color : green_color);
    oil_temp_color = (g_oil_temp < 150) ? blue_color : ((g_oil_temp > 220) ? red_color : green_color);
    oil_pressure_color = (g_oil_pressure > 125 || g_oil_pressure < 25) ? red_color : green_color;

    update_label_if_needed(ui_Label5, g_water_temp, water_temp_color);
    update_label_if_needed(ui_Label6, g_fuel_pressure, fuel_pressure_color);
    update_label_if_needed(ui_Label7, g_oil_temp, oil_temp_color);
    update_label_if_needed(ui_Label8, g_oil_pressure, oil_pressure_color);

}

void update_fuel_arc(){

    // Update for fuel gauge color, slows down FPS a little
    lv_color_t new_color = green_color;
    new_color = (g_fuel_level < 20) ? red_color : ((g_fuel_level < 35) ? orange_color : green_color) ;

    // Only update color if changed
    lv_color_t old_color = lv_obj_get_style_arc_color(fuel_arc, LV_PART_INDICATOR);
    if (old_color.full != new_color.full) {
        lv_obj_set_style_arc_color(fuel_arc, new_color, LV_PART_INDICATOR);
    }

    if (g_fuel_level < 15) {
        //Blinking handled in low_fuel_blink_timer
    } else if (g_fuel_level < 20) {
        lv_obj_clear_flag(low_gas_img, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(low_gas_img, LV_OBJ_FLAG_HIDDEN);
    }

    lv_arc_set_value(fuel_arc, g_fuel_level);
}

void start_espnow(void) {
    if(!espNowSetup) {
        esp_now_setup();
        espNowSetup = true;
    }

    espNowRunning = true;

    uint32_t now = lv_tick_get();
    for(int i=0;i<4;i++) {
        brakeLastSeen[i] = now - 5000;
        disconnectStart[i] = now - 5000;
        brakeConnectedState[i] = false;
    }

    ESP_LOGI(
        TAG,
        "ESP-NOW ENABLED"
    );
}

void stop_espnow(void) {
    espNowRunning = false;

    ESP_LOGI(
        TAG,
        "ESP-NOW DISABLED"
    );
}


void next_screen(void) {
    if(currentScreen == SCREEN_GAUGES) {
        if(!screen2_initialized) {
            ui_Screen2_screen_init();
            screen2_initialized = true;
        }

        start_espnow();

        currentScreen = SCREEN_BRAKES;

        lv_scr_load(ui_Screen2);

        // Slower tire screen refresh
        lv_timer_set_period(
            gaugeTimer,
            150
        );
    }
    else {
        stop_espnow();

        currentScreen = SCREEN_GAUGES;

        lv_scr_load(ui_Screen1);

        // Faster gauge screen refresh
        lv_timer_set_period(
            gaugeTimer,
            50
        );
    }
}


void handle_buttons(void){

    static bool lastState = true;

    static uint32_t lastPress = 0;

    bool pressed =
        gpio_get_level(BTN_NEXT_SCREEN);

    uint32_t now =
        xTaskGetTickCount();

    // Falling edge detect
    if(
        lastState == true &&
        pressed == false
    ){

        // debounce
        if(
            now - lastPress >
            pdMS_TO_TICKS(15)
        ){

            next_screen();

            lastPress = now;
        }
    }

    lastState = pressed;
}




void low_fuel_blink_timer(lv_timer_t * t) {
    if (g_fuel_level >= 15) {
        return;
    }

    low_fuel_blink_state = !low_fuel_blink_state;

    if (low_fuel_blink_state) {
        lv_obj_clear_flag(low_gas_img, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(low_gas_img, LV_OBJ_FLAG_HIDDEN);
    }
}

void gauge_timer(lv_timer_t * t){ 
    switch(currentScreen){
        case SCREEN_GAUGES:
            update_gauge_values();
            break;

        case SCREEN_BRAKES:
            update_brake_screen();
            break;
    }
}

void arc_timer(lv_timer_t * t){
    update_fuel_arc();
}

void brake_timer(lv_timer_t * t)
{
updateRotor(&brake_FL, g_brake_fl);
updateRotor(&brake_FR, g_brake_fr);
updateRotor(&brake_RL, g_brake_rl);
updateRotor(&brake_RR, g_brake_rr);
}

void button_timer(lv_timer_t * t){
    handle_buttons();
}


void app_main(void){

    I2C_Init();
    EXIO_Init();
    LCD_Init();
    Touch_Init();
    LVGL_Init();

    rtc_cpu_freq_config_t config;
    rtc_clk_cpu_freq_mhz_to_config(80, &config);
    rtc_clk_cpu_freq_set_config(&config);

    init_label_styles();
    
    ui_init();

    uart_init();

    xTaskCreate(
        uart_rx_task,
        "uart_rx",
        4096,
        NULL,
        5,
        NULL
    );

    gaugeTimer =
        lv_timer_create(
            gauge_timer,
            50,
            NULL
        );
    lv_timer_create(button_timer, 10, NULL);
    
    lv_timer_create(arc_timer, 1000, NULL);
    lv_timer_create(low_fuel_blink_timer, 400, NULL);

    Set_Backlight(0); 
    vTaskDelay(pdMS_TO_TICKS(250)); 
    Set_Backlight(50);
}