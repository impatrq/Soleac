#include "lcd_control.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_lcd_axs15231b.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_touch.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#define TAG "LCD"
#define LCD_HOST SPI2_HOST
#define LCD_QSPI_CS GPIO_NUM_12
#define LCD_QSPI_CLK GPIO_NUM_5
#define LCD_QSPI_D0 GPIO_NUM_1
#define LCD_QSPI_D1 GPIO_NUM_2
#define LCD_QSPI_D2 GPIO_NUM_3
#define LCD_QSPI_D3 GPIO_NUM_4
#define LCD_BACKLIGHT_PIN GPIO_NUM_6
#define BOARD_I2C_PORT I2C_NUM_0
#define BOARD_I2C_SDA GPIO_NUM_8
#define BOARD_I2C_SCL GPIO_NUM_7
#define BOARD_I2C_SPEED_HZ 400000
#define TCA9554_ADDR 0x20
#define TCA9554_REG_OUTPUT 0x01
#define TCA9554_REG_CONFIG 0x03
#define TCA9554_LCD_RESET_PIN 1
#define TOUCH_MAX_POINTS 2
#define TOUCH_DEBOUNCE_MS 600
#define TOUCH_DEMO_Y_MIN 400
#define TOUCH_DEMO_ENABLE 0
#define LCD_TOUCH_ENABLE 0
#define LCD_EXTERNAL_RESET_ENABLE 0
#define LCD_H_RES 320
#define LCD_V_RES 480
#define LCD_TRANSFER_LINES 40
#define LCD_REFRESH_MS 250
#define LCD_FONT_WIDTH 5
#define LCD_FONT_SPACING 1
#define LCD_FONT_HEIGHT 8
#define LCD_MAX_TEXT_SCALE 5
// Tipografia tecnica limpia: matriz bitmap monoespaciada 5x8.
// La negrita se aplica solamente a los titulos que la solicitan.
#define LCD_INDUSTRIAL_FONT 0
#define LCD_COLOR_BLACK 0x0000
#define LCD_COLOR_WHITE 0xFFFF
#define LCD_COLOR_GREEN 0x07E0
#define LCD_COLOR_RED 0xF800
#define LCD_COLOR_YELLOW 0xFFE0
#define LCD_COLOR_BLUE 0x001F
#define LCD_COLOR_CYAN 0x07FF
#define LCD_COLOR_GRAY 0x39E7
#define LCD_COLOR_DARK 0x0841
#define LCD_DRAW_LOGS 0
#define LCD_RUN_BOOT_DIAGNOSTICS 0
#define LCD_HOLD_DIAGNOSTICS 0
// Mantener el renderizado habilitado para mostrar los datos del MEMS durante
// la prueba; el diagnostico grafico de arranque permanece desactivado.
#define LCD_RUNTIME_RENDER_ENABLE 1
#define LCD_BOOT_SCREEN_ENABLE 0
#define LCD_QSPI_CLOCK_HZ (32 * 1000 * 1000)
#define LCD_QSPI_CMD_WRITE 0x02
#define LCD_QSPI_COLOR_WRITE 0x32
#define LCD_QSPI_COLOR_ADDR 0x003C00
#define LCD_QSPI_PARAM_MAX 64
#define LCD_QSPI_PIXEL_CHUNK_PIXELS (LCD_H_RES * LCD_TRANSFER_LINES)

static spi_device_handle_t lcd_spi = NULL;
#if LCD_TOUCH_ENABLE
static esp_lcd_panel_io_handle_t touch_io = NULL;
static esp_lcd_touch_handle_t touch_panel = NULL;
#endif
static i2c_master_bus_handle_t board_i2c_bus = NULL;
static i2c_master_dev_handle_t tca9554_dev = NULL;
static bool lcd_disponible = false;
#if LCD_TOUCH_ENABLE
static bool touch_disponible = false;
#endif
static bool lcd_diagnostico_fijo = false;
static uint32_t ultimo_refresco_lcd_ms = 0;
#if TOUCH_DEMO_ENABLE
static uint32_t ultimo_touch_demo_ms = 0;
#endif
static uint16_t lcd_draw_buffer[LCD_H_RES * LCD_TRANSFER_LINES];
static uint16_t lcd_char_buffer[
    LCD_FONT_HEIGHT * (LCD_FONT_WIDTH + LCD_FONT_SPACING) * LCD_MAX_TEXT_SCALE * LCD_MAX_TEXT_SCALE
];
static uint16_t *lcd_framebuffer = NULL;
static uint8_t lcd_qspi_param_buffer[LCD_QSPI_PARAM_MAX];
static uint8_t lcd_qspi_pixel_buffer[LCD_QSPI_PIXEL_CHUNK_PIXELS * sizeof(uint16_t)];
static bool lcd_batch_mode = false;
static uint8_t tca9554_output_state = 0xFF;
static uint8_t tca9554_config_state = 0xFF;

static int lcd_draw_call_count = 0;
static int lcd_fill_call_count = 0;
static int lcd_string_call_count = 0;

static const uint8_t *lcd_glyph(char c)
{
    static const uint8_t blank[LCD_FONT_WIDTH] = {0x00, 0x00, 0x00, 0x00, 0x00};
    static const uint8_t digits[10][LCD_FONT_WIDTH] = {
        {0x3E, 0x51, 0x49, 0x45, 0x3E},
        {0x00, 0x42, 0x7F, 0x40, 0x00},
        {0x42, 0x61, 0x51, 0x49, 0x46},
        {0x21, 0x41, 0x45, 0x4B, 0x31},
        {0x18, 0x14, 0x12, 0x7F, 0x10},
        {0x27, 0x45, 0x45, 0x45, 0x39},
        {0x3C, 0x4A, 0x49, 0x49, 0x30},
        {0x01, 0x71, 0x09, 0x05, 0x03},
        {0x36, 0x49, 0x49, 0x49, 0x36},
        {0x06, 0x49, 0x49, 0x29, 0x1E},
    };
    static const uint8_t letters[26][LCD_FONT_WIDTH] = {
        {0x7E, 0x11, 0x11, 0x11, 0x7E},
        {0x7F, 0x49, 0x49, 0x49, 0x36},
        {0x3E, 0x41, 0x41, 0x41, 0x22},
        {0x7F, 0x41, 0x41, 0x22, 0x1C},
        {0x7F, 0x49, 0x49, 0x49, 0x41},
        {0x7F, 0x09, 0x09, 0x09, 0x01},
        {0x3E, 0x41, 0x49, 0x49, 0x7A},
        {0x7F, 0x08, 0x08, 0x08, 0x7F},
        {0x00, 0x41, 0x7F, 0x41, 0x00},
        {0x20, 0x40, 0x41, 0x3F, 0x01},
        {0x7F, 0x08, 0x14, 0x22, 0x41},
        {0x7F, 0x40, 0x40, 0x40, 0x40},
        {0x7F, 0x02, 0x0C, 0x02, 0x7F},
        {0x7F, 0x04, 0x08, 0x10, 0x7F},
        {0x3E, 0x41, 0x41, 0x41, 0x3E},
        {0x7F, 0x09, 0x09, 0x09, 0x06},
        {0x3E, 0x41, 0x51, 0x21, 0x5E},
        {0x7F, 0x09, 0x19, 0x29, 0x46},
        {0x46, 0x49, 0x49, 0x49, 0x31},
        {0x01, 0x01, 0x7F, 0x01, 0x01},
        {0x3F, 0x40, 0x40, 0x40, 0x3F},
        // V: el glifo anterior dibujaba una W y provocaba "REWISAR"/"WALIDO".
        {0x1F, 0x20, 0x40, 0x20, 0x1F},
        {0x7F, 0x08, 0x08, 0x08, 0x7F},
        // X: diagonales claras para que no parezca una N en la LCD.
        {0x63, 0x14, 0x08, 0x14, 0x63},
        {0x7F, 0x02, 0x0C, 0x02, 0x7F},
        {0x7E, 0x11, 0x11, 0x11, 0x7E},
    };
    static const uint8_t colon[LCD_FONT_WIDTH] = {0x00, 0x36, 0x36, 0x00, 0x00};
    static const uint8_t dot[LCD_FONT_WIDTH] = {0x00, 0x60, 0x60, 0x00, 0x00};
    static const uint8_t minus[LCD_FONT_WIDTH] = {0x08, 0x08, 0x08, 0x08, 0x08};
    static const uint8_t slash[LCD_FONT_WIDTH] = {0x20, 0x10, 0x08, 0x04, 0x02};
    static const uint8_t greater[LCD_FONT_WIDTH] = {0x00, 0x41, 0x22, 0x14, 0x08};
    static const uint8_t less[LCD_FONT_WIDTH] = {0x08, 0x14, 0x22, 0x41, 0x00};
    static const uint8_t percent[LCD_FONT_WIDTH] = {0x62, 0x64, 0x08, 0x13, 0x23};

    if (c >= '0' && c <= '9') {
        return digits[c - '0'];
    }
    if (c >= 'A' && c <= 'Z') {
        return letters[c - 'A'];
    }

    switch (c) {
        case ':':
            return colon;
        case '.':
            return dot;
        case '-':
            return minus;
        case '/':
            return slash;
        case '%':
            return percent;
        case '>':
            return greater;
        case '<':
            return less;
        default:
            return blank;
    }
}

static esp_err_t init_board_i2c(void)
{
    if (board_i2c_bus != NULL) {
        return ESP_OK;
    }

    const i2c_master_bus_config_t bus_config = {
        .i2c_port = BOARD_I2C_PORT,
        .sda_io_num = BOARD_I2C_SDA,
        .scl_io_num = BOARD_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags = {
            .enable_internal_pullup = true,
        },
    };

    esp_err_t ret = i2c_new_master_bus(&bus_config, &board_i2c_bus);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "No se pudo inicializar el bus I2C de placa: %s", esp_err_to_name(ret));
        board_i2c_bus = NULL;
    }
    return ret;
}

static esp_err_t tca9554_init(void)
{
    if (tca9554_dev != NULL) {
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(init_board_i2c(), TAG, "Fallo I2C de placa");

    const i2c_device_config_t dev_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = TCA9554_ADDR,
        .scl_speed_hz = BOARD_I2C_SPEED_HZ,
    };

    esp_err_t ret = i2c_master_bus_add_device(board_i2c_bus, &dev_config, &tca9554_dev);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "No se pudo agregar TCA9554 al bus I2C: %s", esp_err_to_name(ret));
        tca9554_dev = NULL;
    }
    return ret;
}

static esp_err_t tca9554_write_reg(uint8_t reg, uint8_t value)
{
    uint8_t data[2] = {reg, value};
    ESP_RETURN_ON_ERROR(tca9554_init(), TAG, "TCA9554 no disponible");
    return i2c_master_transmit(tca9554_dev, data, sizeof(data), 100);
}

static esp_err_t tca9554_set_pin_mode_output(uint8_t pin)
{
    tca9554_config_state &= (uint8_t)~(1U << pin);
    return tca9554_write_reg(TCA9554_REG_CONFIG, tca9554_config_state);
}

static esp_err_t tca9554_write_pin(uint8_t pin, bool level)
{
    if (level) {
        tca9554_output_state |= (uint8_t)(1U << pin);
    } else {
        tca9554_output_state &= (uint8_t)~(1U << pin);
    }
    return tca9554_write_reg(TCA9554_REG_OUTPUT, tca9554_output_state);
}

static esp_err_t reset_waveshare_lcd_expander(void)
{
    ESP_RETURN_ON_ERROR(tca9554_set_pin_mode_output(TCA9554_LCD_RESET_PIN), TAG, "Fallo config TCA9554 LCD_RST");
    ESP_RETURN_ON_ERROR(tca9554_write_pin(TCA9554_LCD_RESET_PIN, true), TAG, "Fallo LCD_RST high");
    vTaskDelay(pdMS_TO_TICKS(10));
    ESP_RETURN_ON_ERROR(tca9554_write_pin(TCA9554_LCD_RESET_PIN, false), TAG, "Fallo LCD_RST low");
    vTaskDelay(pdMS_TO_TICKS(10));
    ESP_RETURN_ON_ERROR(tca9554_write_pin(TCA9554_LCD_RESET_PIN, true), TAG, "Fallo LCD_RST release");
    vTaskDelay(pdMS_TO_TICKS(200));
    return ESP_OK;
}

static const axs15231b_lcd_init_cmd_t waveshare_qspi_init_cmds[] = {
    {0xBB, (uint8_t[]){0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x5A, 0xA5}, 8, 0},
    {0xA0, (uint8_t[]){0xC0, 0x10, 0x00, 0x02, 0x00, 0x00, 0x04, 0x3F, 0x20, 0x05, 0x3F, 0x3F, 0x00, 0x00, 0x00, 0x00, 0x00}, 17, 0},
    {0xA2, (uint8_t[]){0x30, 0x3C, 0x24, 0x14, 0xD0, 0x20, 0xFF, 0xE0, 0x40, 0x19, 0x80, 0x80, 0x80, 0x20, 0xF9, 0x10, 0x02, 0xFF, 0xFF, 0xF0, 0x90, 0x01, 0x32, 0xA0, 0x91, 0xE0, 0x20, 0x7F, 0xFF, 0x00, 0x5A}, 31, 0},
    {0xD0, (uint8_t[]){0xE0, 0x40, 0x51, 0x24, 0x08, 0x05, 0x10, 0x01, 0x20, 0x15, 0xC2, 0x42, 0x22, 0x22, 0xAA, 0x03, 0x10, 0x12, 0x60, 0x14, 0x1E, 0x51, 0x15, 0x00, 0x8A, 0x20, 0x00, 0x03, 0x3A, 0x12}, 30, 0},
    {0xA3, (uint8_t[]){0xA0, 0x06, 0xAA, 0x00, 0x08, 0x02, 0x0A, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x00, 0x55, 0x55}, 22, 0},
    {0xC1, (uint8_t[]){0x31, 0x04, 0x02, 0x02, 0x71, 0x05, 0x24, 0x55, 0x02, 0x00, 0x41, 0x00, 0x53, 0xFF, 0xFF, 0xFF, 0x4F, 0x52, 0x00, 0x4F, 0x52, 0x00, 0x45, 0x3B, 0x0B, 0x02, 0x0D, 0x00, 0xFF, 0x40}, 30, 0},
    {0xC3, (uint8_t[]){0x00, 0x00, 0x00, 0x50, 0x03, 0x00, 0x00, 0x00, 0x01, 0x80, 0x01}, 11, 0},
    {0xC4, (uint8_t[]){0x00, 0x24, 0x33, 0x80, 0x00, 0xEA, 0x64, 0x32, 0xC8, 0x64, 0xC8, 0x32, 0x90, 0x90, 0x11, 0x06, 0xDC, 0xFA, 0x00, 0x00, 0x80, 0xFE, 0x10, 0x10, 0x00, 0x0A, 0x0A, 0x44, 0x50}, 29, 0},
    {0xC5, (uint8_t[]){0x18, 0x00, 0x00, 0x03, 0xFE, 0x3A, 0x4A, 0x20, 0x30, 0x10, 0x88, 0xDE, 0x0D, 0x08, 0x0F, 0x0F, 0x01, 0x3A, 0x4A, 0x20, 0x10, 0x10, 0x00}, 23, 0},
    {0xC6, (uint8_t[]){0x05, 0x0A, 0x05, 0x0A, 0x00, 0xE0, 0x2E, 0x0B, 0x12, 0x22, 0x12, 0x22, 0x01, 0x03, 0x00, 0x3F, 0x6A, 0x18, 0xC8, 0x22}, 20, 0},
    {0xC7, (uint8_t[]){0x50, 0x32, 0x28, 0x00, 0xA2, 0x80, 0x8F, 0x00, 0x80, 0xFF, 0x07, 0x11, 0x9C, 0x67, 0xFF, 0x24, 0x0C, 0x0D, 0x0E, 0x0F}, 20, 0},
    {0xC9, (uint8_t[]){0x33, 0x44, 0x44, 0x01}, 4, 0},
    {0xCF, (uint8_t[]){0x2C, 0x1E, 0x88, 0x58, 0x13, 0x18, 0x56, 0x18, 0x1E, 0x68, 0x88, 0x00, 0x65, 0x09, 0x22, 0xC4, 0x0C, 0x77, 0x22, 0x44, 0xAA, 0x55, 0x08, 0x08, 0x12, 0xA0, 0x08}, 27, 0},
    {0xD5, (uint8_t[]){0x40, 0x8E, 0x8D, 0x01, 0x35, 0x04, 0x92, 0x74, 0x04, 0x92, 0x74, 0x04, 0x08, 0x6A, 0x04, 0x46, 0x03, 0x03, 0x03, 0x03, 0x82, 0x01, 0x03, 0x00, 0xE0, 0x51, 0xA1, 0x00, 0x00, 0x00}, 30, 0},
    {0xD6, (uint8_t[]){0x10, 0x32, 0x54, 0x76, 0x98, 0xBA, 0xDC, 0xFE, 0x93, 0x00, 0x01, 0x83, 0x07, 0x07, 0x00, 0x07, 0x07, 0x00, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x00, 0x84, 0x00, 0x20, 0x01, 0x00}, 30, 0},
    {0xD7, (uint8_t[]){0x03, 0x01, 0x0B, 0x09, 0x0F, 0x0D, 0x1E, 0x1F, 0x18, 0x1D, 0x1F, 0x19, 0x40, 0x8E, 0x04, 0x00, 0x20, 0xA0, 0x1F}, 19, 0},
    {0xD8, (uint8_t[]){0x02, 0x00, 0x0A, 0x08, 0x0E, 0x0C, 0x1E, 0x1F, 0x18, 0x1D, 0x1F, 0x19}, 12, 0},
    {0xD9, (uint8_t[]){0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F}, 12, 0},
    {0xDD, (uint8_t[]){0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F}, 12, 0},
    {0xDF, (uint8_t[]){0x44, 0x73, 0x4B, 0x69, 0x00, 0x0A, 0x02, 0x90}, 8, 0},
    {0xE0, (uint8_t[]){0x3B, 0x28, 0x10, 0x16, 0x0C, 0x06, 0x11, 0x28, 0x5C, 0x21, 0x0D, 0x35, 0x13, 0x2C, 0x33, 0x28, 0x0D}, 17, 0},
    {0xE1, (uint8_t[]){0x37, 0x28, 0x10, 0x16, 0x0B, 0x06, 0x11, 0x28, 0x5C, 0x21, 0x0D, 0x35, 0x14, 0x2C, 0x33, 0x28, 0x0F}, 17, 0},
    {0xE2, (uint8_t[]){0x3B, 0x07, 0x12, 0x18, 0x0E, 0x0D, 0x17, 0x35, 0x44, 0x32, 0x0C, 0x14, 0x14, 0x36, 0x3A, 0x2F, 0x0D}, 17, 0},
    {0xE3, (uint8_t[]){0x37, 0x07, 0x12, 0x18, 0x0E, 0x0D, 0x17, 0x35, 0x44, 0x32, 0x0C, 0x14, 0x14, 0x36, 0x32, 0x2F, 0x0F}, 17, 0},
    {0xE4, (uint8_t[]){0x3B, 0x07, 0x12, 0x18, 0x0E, 0x0D, 0x17, 0x39, 0x44, 0x2E, 0x0C, 0x14, 0x14, 0x36, 0x3A, 0x2F, 0x0D}, 17, 0},
    {0xE5, (uint8_t[]){0x37, 0x07, 0x12, 0x18, 0x0E, 0x0D, 0x17, 0x39, 0x44, 0x2E, 0x0C, 0x14, 0x14, 0x36, 0x3A, 0x2F, 0x0F}, 17, 0},
    {0xA4, (uint8_t[]){0x85, 0x85, 0x95, 0x82, 0xAF, 0xAA, 0xAA, 0x80, 0x10, 0x30, 0x40, 0x40, 0x20, 0xFF, 0x60, 0x30}, 16, 0},
    {0xA4, (uint8_t[]){0x85, 0x85, 0x95, 0x85}, 4, 0},
    {0xBB, (uint8_t[]){0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, 8, 0},
    {0x11, (uint8_t[]){0x00}, 0, 120},
    {0x29, (uint8_t[]){0x00}, 0, 100},
    {0x2C, (uint8_t[]){0x00, 0x00, 0x00, 0x00}, 4, 0},
};

static inline void lcd_qspi_cs_low(void)
{
    gpio_set_level(LCD_QSPI_CS, 0);
}

static inline void lcd_qspi_cs_high(void)
{
    gpio_set_level(LCD_QSPI_CS, 1);
}

static esp_err_t lcd_qspi_transmit(
    uint32_t flags,
    uint8_t cmd,
    uint32_t addr,
    const void *data,
    size_t data_bits,
    bool variable_bus_width)
{
    spi_transaction_ext_t trans = {0};

    trans.base.flags = flags;
    trans.base.cmd = cmd;
    trans.base.addr = addr;
    trans.base.length = data_bits;
    trans.base.tx_buffer = data;

    if (variable_bus_width) {
        trans.command_bits = 0;
        trans.address_bits = 0;
        trans.dummy_bits = 0;
    }

    return spi_device_polling_transmit(lcd_spi, (spi_transaction_t *)&trans);
}

static esp_err_t lcd_qspi_write_cmd(uint8_t cmd, const uint8_t *data, size_t data_len)
{
    if (lcd_spi == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (data_len > sizeof(lcd_qspi_param_buffer)) {
        return ESP_ERR_INVALID_ARG;
    }

    const void *tx_data = NULL;
    size_t tx_bits = 0;
    if (data_len > 0) {
        memcpy(lcd_qspi_param_buffer, data, data_len);
        tx_data = lcd_qspi_param_buffer;
        tx_bits = data_len * 8U;
    }

    lcd_qspi_cs_low();
    esp_err_t ret = lcd_qspi_transmit(
        SPI_TRANS_MULTILINE_CMD | SPI_TRANS_MULTILINE_ADDR,
        LCD_QSPI_CMD_WRITE,
        ((uint32_t)cmd) << 8,
        tx_data,
        tx_bits,
        false);
    lcd_qspi_cs_high();

    return ret;
}

static esp_err_t lcd_qspi_write_cmd_u16_pair(uint8_t cmd, uint16_t a, uint16_t b)
{
    uint8_t data[4] = {
        (uint8_t)(a >> 8),
        (uint8_t)(a & 0xFF),
        (uint8_t)(b >> 8),
        (uint8_t)(b & 0xFF),
    };

    return lcd_qspi_write_cmd(cmd, data, sizeof(data));
}

static esp_err_t lcd_qspi_set_window(int x_start, int y_start, int x_end, int y_end)
{
    ESP_RETURN_ON_ERROR(lcd_qspi_write_cmd_u16_pair(0x2A, (uint16_t)x_start, (uint16_t)(x_end - 1)), TAG, "CASET fallo");
    ESP_RETURN_ON_ERROR(lcd_qspi_write_cmd_u16_pair(0x2B, (uint16_t)y_start, (uint16_t)(y_end - 1)), TAG, "RASET fallo");
    return lcd_qspi_write_cmd(0x2C, NULL, 0);
}

static esp_err_t lcd_qspi_push_pixels(int x_start, int y_start, int x_end, int y_end, const uint16_t *src)
{
    int width = x_end - x_start;
    int height = y_end - y_start;
    size_t total_pixels = (size_t)width * (size_t)height;
    size_t sent_pixels = 0;
    bool first_chunk = true;

    if (lcd_spi == NULL || src == NULL || width <= 0 || height <= 0) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = lcd_qspi_set_window(x_start, y_start, x_end, y_end);
    if (err == ESP_OK) {
        lcd_qspi_cs_low();
        while (sent_pixels < total_pixels) {
            size_t chunk_pixels = total_pixels - sent_pixels;
            if (chunk_pixels > LCD_QSPI_PIXEL_CHUNK_PIXELS) {
                chunk_pixels = LCD_QSPI_PIXEL_CHUNK_PIXELS;
            }

            for (size_t i = 0; i < chunk_pixels; i++) {
                uint16_t color = src[sent_pixels + i];
                lcd_qspi_pixel_buffer[(i * 2U) + 0U] = (uint8_t)(color >> 8);
                lcd_qspi_pixel_buffer[(i * 2U) + 1U] = (uint8_t)(color & 0xFF);
            }

            if (first_chunk) {
                err = lcd_qspi_transmit(
                    SPI_TRANS_MODE_QIO,
                    LCD_QSPI_COLOR_WRITE,
                    LCD_QSPI_COLOR_ADDR,
                    lcd_qspi_pixel_buffer,
                    chunk_pixels * 16U,
                    false);
                first_chunk = false;
            } else {
                err = lcd_qspi_transmit(
                    SPI_TRANS_MODE_QIO | SPI_TRANS_VARIABLE_CMD | SPI_TRANS_VARIABLE_ADDR | SPI_TRANS_VARIABLE_DUMMY,
                    0,
                    0,
                    lcd_qspi_pixel_buffer,
                    chunk_pixels * 16U,
                    true);
            }

            if (err != ESP_OK) {
                break;
            }
            sent_pixels += chunk_pixels;
        }
        lcd_qspi_cs_high();
    }

    return err;
}

static esp_err_t lcd_flush_framebuffer(void)
{
    if (lcd_framebuffer == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    return lcd_qspi_push_pixels(0, 0, LCD_H_RES, LCD_V_RES, lcd_framebuffer);
}

static void lcd_framebuffer_blit(int x_start, int y_start, int x_end, int y_end, const uint16_t *src)
{
    int width = x_end - x_start;
    int height = y_end - y_start;

    if (lcd_framebuffer == NULL || src == NULL || width <= 0 || height <= 0) {
        return;
    }

    for (int row = 0; row < height; row++) {
        memcpy(
            &lcd_framebuffer[((y_start + row) * LCD_H_RES) + x_start],
            &src[row * width],
            (size_t)width * sizeof(uint16_t));
    }
}

static bool lcd_draw_bitmap_checked(int x_start, int y_start, int x_end, int y_end, const void *color_data)
{
    static int fallos_consecutivos = 0;
    lcd_draw_call_count++;
    const uint16_t *src = (const uint16_t *)color_data;

#if LCD_DRAW_LOGS
    int width = x_end - x_start;
    int height = y_end - y_start;
    ESP_LOGI(
        TAG,
        "draw_bitmap #%d: area=(%d,%d)-(%d,%d) size=%dx%d bytes=%u buffer=%p",
        lcd_draw_call_count,
        x_start,
        y_start,
        x_end,
        y_end,
        width,
        height,
        (unsigned)((size_t)width * (size_t)height * sizeof(uint16_t)),
        color_data);
#endif

    esp_err_t err = ESP_OK;
    if (lcd_framebuffer != NULL) {
        lcd_framebuffer_blit(x_start, y_start, x_end, y_end, src);
        if (!lcd_batch_mode) {
            err = lcd_flush_framebuffer();
        }
    } else {
        err = lcd_qspi_push_pixels(x_start, y_start, x_end, y_end, src);
    }

    if (err != ESP_OK) {
        fallos_consecutivos++;
        ESP_LOGW(TAG, "Fallo dibujo LCD (%d/10): %s", fallos_consecutivos, esp_err_to_name(err));
        if (fallos_consecutivos >= 10) {
            ESP_LOGE(TAG, "LCD deshabilitada tras 10 fallos consecutivos.");
            lcd_disponible = false;
        }
        return false;
    }
#if LCD_DRAW_LOGS
    ESP_LOGI(TAG, "draw_bitmap #%d OK: datos QSPI enviados al panel (%u bytes)", lcd_draw_call_count, (unsigned)bytes);
#endif
    fallos_consecutivos = 0;
    return true;
}

static void lcd_fill_rect(int x, int y, int width, int height, uint16_t color)
{
    lcd_fill_call_count++;

#if LCD_DRAW_LOGS
    ESP_LOGI(
        TAG,
        "lcd_fill_rect #%d: req x=%d y=%d w=%d h=%d color=0x%04X",
        lcd_fill_call_count,
        x,
        y,
        width,
        height,
        color);
#endif

    if (!lcd_disponible || lcd_spi == NULL || width <= 0 || height <= 0) {
#if LCD_DRAW_LOGS
        ESP_LOGW(TAG, "lcd_fill_rect #%d omitido: lcd=%d spi=%p w=%d h=%d", lcd_fill_call_count, lcd_disponible, lcd_spi, width, height);
#endif
        return;
    }

    if (x < 0) {
        width += x;
        x = 0;
    }
    if (y < 0) {
        height += y;
        y = 0;
    }
    if ((x + width) > LCD_H_RES) {
        width = LCD_H_RES - x;
    }
    if ((y + height) > LCD_V_RES) {
        height = LCD_V_RES - y;
    }
    if (width <= 0 || height <= 0) {
#if LCD_DRAW_LOGS
        ESP_LOGW(
            TAG,
            "lcd_fill_rect #%d fuera de pantalla: req=(%d,%d %dx%d)",
            lcd_fill_call_count,
            x,
            y,
            width,
            height);
#endif
        return;
    }

    int max_rows = (int)(sizeof(lcd_draw_buffer) / sizeof(lcd_draw_buffer[0])) / width;
    if (max_rows <= 0) {
#if LCD_DRAW_LOGS
        ESP_LOGE(TAG, "lcd_fill_rect #%d sin buffer: width=%d buffer_pixels=%u", lcd_fill_call_count, width, (unsigned)(sizeof(lcd_draw_buffer) / sizeof(lcd_draw_buffer[0])));
#endif
        return;
    }

#if LCD_DRAW_LOGS
    ESP_LOGI(
        TAG,
        "lcd_fill_rect #%d clip x=%d y=%d w=%d h=%d max_rows=%d",
        lcd_fill_call_count,
        x,
        y,
        width,
        height,
        max_rows);
#endif

    int drawn_rows = 0;
    while (drawn_rows < height) {
        int rows = height - drawn_rows;
        if (rows > max_rows) {
            rows = max_rows;
        }

        int pixels = width * rows;
        for (int i = 0; i < pixels; i++) {
            lcd_draw_buffer[i] = color;
        }

        if (!lcd_draw_bitmap_checked(x, y + drawn_rows, x + width, y + drawn_rows + rows, lcd_draw_buffer)) {
            return;
        }
        drawn_rows += rows;
    }
}

static void lcd_draw_char(int x, int y, char c, uint16_t color, uint16_t bg, int scale, bool bold)
{
    const uint8_t *glyph = lcd_glyph(c);
    int char_width;
    int char_height;

    if (!lcd_disponible || lcd_spi == NULL) {
        return;
    }
    if (scale < 1) {
        scale = 1;
    }
    if (scale > LCD_MAX_TEXT_SCALE) {
        scale = LCD_MAX_TEXT_SCALE;
    }

#if LCD_INDUSTRIAL_FONT
    // El estilo industrial usa siempre trazos gruesos y cuadrados.
    // El parametro bold sigue siendo compatible, pero ya no es necesario
    // activarlo individualmente en cada texto.
    bold = true;
#endif

    char_width = (LCD_FONT_WIDTH + LCD_FONT_SPACING) * scale;
    char_height = LCD_FONT_HEIGHT * scale;
    if (x < 0 || y < 0 || (x + char_width) > LCD_H_RES || (y + char_height) > LCD_V_RES) {
        return;
    }

    for (int row_px = 0; row_px < char_height; row_px++) {
        int row = row_px / scale;
        for (int col_px = 0; col_px < char_width; col_px++) {
            int col = col_px / scale;
            bool lit = (col < LCD_FONT_WIDTH) && ((glyph[col] & (1U << row)) != 0);
            if (bold && !lit && col > 0 && col < LCD_FONT_WIDTH) {
                // "Negrito": engrosamos el trazo encendiendo tambien el pixel vecino de la izquierda.
                lit = (glyph[col - 1] & (1U << row)) != 0;
            }
            lcd_char_buffer[(row_px * char_width) + col_px] = lit ? color : bg;
        }
    }

    (void)lcd_draw_bitmap_checked(x, y, x + char_width, y + char_height, lcd_char_buffer);
}

static int lcd_text_width(const char *text, int scale)
{
    if (text == NULL) {
        return 0;
    }
    if (scale < 1) {
        scale = 1;
    }
    if (scale > LCD_MAX_TEXT_SCALE) {
        scale = LCD_MAX_TEXT_SCALE;
    }
    return (int)strlen(text) * (LCD_FONT_WIDTH + LCD_FONT_SPACING) * scale;
}

static void lcd_draw_string_ex(int x, int y, const char *text, uint16_t color, uint16_t bg, int scale, bool bold)
{
    int step;
    lcd_string_call_count++;

    if (scale < 1) {
        scale = 1;
    }
    if (scale > LCD_MAX_TEXT_SCALE) {
        scale = LCD_MAX_TEXT_SCALE;
    }

    step = (LCD_FONT_WIDTH + LCD_FONT_SPACING) * scale;
#if LCD_DRAW_LOGS
    ESP_LOGI(
        TAG,
        "lcd_draw_string #%d: x=%d y=%d text=\"%s\" color=0x%04X bg=0x%04X scale=%d step=%d bold=%d",
        lcd_string_call_count,
        x,
        y,
        text ? text : "(null)",
        color,
        bg,
        scale,
        step,
        bold);
#endif
    if (text == NULL) {
        return;
    }
    while (*text != '\0' && x <= (LCD_H_RES - step)) {
        lcd_draw_char(x, y, *text, color, bg, scale, bold);
        x += step;
        text++;
    }
}

static void lcd_draw_string(int x, int y, const char *text, uint16_t color, uint16_t bg, int scale)
{
    lcd_draw_string_ex(x, y, text, color, bg, scale, false);
}

static void lcd_draw_string_centered(int y, const char *text, uint16_t color, uint16_t bg, int scale, bool bold)
{
    int width = lcd_text_width(text, scale);
    int x = (LCD_H_RES - width) / 2;
    if (x < 0) {
        x = 0;
    }
    lcd_draw_string_ex(x, y, text, color, bg, scale, bold);
}

static void lcd_draw_meter(int x, int y, int width, int height, float value, float max_value, uint16_t color)
{
    int filled = 0;

    lcd_fill_rect(x, y, width, height, LCD_COLOR_DARK);
    if (max_value > 0.0f) {
        float ratio = value / max_value;
        if (ratio < 0.0f) {
            ratio = 0.0f;
        } else if (ratio > 1.0f) {
            ratio = 1.0f;
        }
        filled = (int)((float)width * ratio);
        
    }

    if (filled > 0) {
        lcd_fill_rect(x, y, filled, height, color);
    }
    lcd_fill_rect(x, y, width, 2, LCD_COLOR_GRAY);
    lcd_fill_rect(x, y + height - 2, width, 2, LCD_COLOR_GRAY);
    lcd_fill_rect(x, y, 2, height, LCD_COLOR_GRAY);
    lcd_fill_rect(x + width - 2, y, 2, height, LCD_COLOR_GRAY);
}

static void lcd_show_boot_screen(void)
{
    int scale = 4;
    int line_h = (LCD_FONT_HEIGHT * scale) + 14;
    int y0 = 190;

    lcd_fill_rect(0, 0, LCD_H_RES, LCD_V_RES, LCD_COLOR_WHITE);
    lcd_draw_string_centered(y0, "INICIANDO", LCD_COLOR_BLACK, LCD_COLOR_WHITE, scale, true);
    lcd_draw_string_centered(y0 + line_h, "SOLEAC", LCD_COLOR_BLACK, LCD_COLOR_WHITE, scale, true);
}

#if LCD_TOUCH_ENABLE
static esp_err_t init_waveshare_touch(void)
{
    esp_err_t ret;
    esp_lcd_panel_io_i2c_config_t touch_io_config = ESP_LCD_TOUCH_IO_I2C_AXS15231B_CONFIG();
    const esp_lcd_touch_config_t touch_config = {
        .x_max = LCD_H_RES,
        .y_max = LCD_V_RES,
        .rst_gpio_num = GPIO_NUM_NC,
        .int_gpio_num = GPIO_NUM_NC,
        .levels = {
            .reset = 0,
            .interrupt = 0,
        },
        .flags = {
            .swap_xy = 0,
            .mirror_x = 0,
            .mirror_y = 0,
        },
    };

    touch_io_config.scl_speed_hz = BOARD_I2C_SPEED_HZ;

    ESP_RETURN_ON_ERROR(init_board_i2c(), TAG, "Fallo I2C de placa para touch");

    ret = esp_lcd_new_panel_io_i2c(board_i2c_bus, &touch_io_config, &touch_io);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "No se pudo crear IO I2C del touch AXS15231B: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = esp_lcd_touch_new_i2c_axs15231b(touch_io, &touch_config, &touch_panel);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "No se pudo iniciar touch AXS15231B: %s", esp_err_to_name(ret));
        touch_panel = NULL;
        return ret;
    }

    touch_disponible = true;
    ESP_LOGI(TAG, "Touch AXS15231B listo por I2C 0x%02X", ESP_LCD_TOUCH_IO_I2C_AXS15231B_ADDRESS);
    return ESP_OK;
}
#endif

static esp_err_t init_waveshare_lcd(void)
{
    esp_err_t ret;
#if LCD_EXTERNAL_RESET_ENABLE
    esp_err_t reset_ret;
#endif

    const spi_bus_config_t buscfg = {
        .mosi_io_num = LCD_QSPI_D0,
        .miso_io_num = LCD_QSPI_D1,
        .sclk_io_num = LCD_QSPI_CLK,
        .quadwp_io_num = LCD_QSPI_D2,
        .quadhd_io_num = LCD_QSPI_D3,
        .max_transfer_sz = sizeof(lcd_qspi_pixel_buffer) + 8,
        .flags = SPICOMMON_BUSFLAG_MASTER | SPICOMMON_BUSFLAG_GPIO_PINS,
    };

    const spi_device_interface_config_t devcfg = {
        .command_bits = 8,
        .address_bits = 24,
        .dummy_bits = 0,
        .mode = 0,
        .clock_speed_hz = LCD_QSPI_CLOCK_HZ,
        .spics_io_num = -1,
        .flags = SPI_DEVICE_HALFDUPLEX,
        .queue_size = 1,
    };

    gpio_config_t bk_gpio = {
        .pin_bit_mask = 1ULL << LCD_BACKLIGHT_PIN,
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config_t cs_gpio = {
        .pin_bit_mask = 1ULL << LCD_QSPI_CS,
        .mode = GPIO_MODE_OUTPUT,
    };

    ESP_ERROR_CHECK(gpio_config(&bk_gpio));
    ESP_ERROR_CHECK(gpio_config(&cs_gpio));
    lcd_qspi_cs_high();
    gpio_set_level(LCD_BACKLIGHT_PIN, 1);

    ESP_LOGI(TAG, "========== LCD INIT ==========");
    ESP_LOGI(
        TAG,
        "Config LCD Waveshare: res=%dx%d qspi_host=%d cs=%d clk=%d d0=%d d1=%d d2=%d d3=%d backlight=%d init_cmds=%u",
        LCD_H_RES,
        LCD_V_RES,
        LCD_HOST,
        LCD_QSPI_CS,
        LCD_QSPI_CLK,
        LCD_QSPI_D0,
        LCD_QSPI_D1,
        LCD_QSPI_D2,
        LCD_QSPI_D3,
        LCD_BACKLIGHT_PIN,
        (unsigned)(sizeof(waveshare_qspi_init_cmds) / sizeof(waveshare_qspi_init_cmds[0])));

#if LCD_EXTERNAL_RESET_ENABLE
    reset_ret = reset_waveshare_lcd_expander();
    if (reset_ret != ESP_OK) {
        ESP_LOGW(
            TAG,
            "Reset externo por TCA9554 no disponible (%s). Se continua con reset por software del panel.",
            esp_err_to_name(reset_ret));
    }
#else
    // El TCA9554 puede dejar bloqueada la inicialización si el bus I2C de la
    // placa no responde. El panel también tiene reset por software (0x01),
    // que se ejecuta más abajo y permite que la prueba MEMS continúe.
    ESP_LOGI(TAG, "Reset externo TCA9554 omitido; se usa reset por software del LCD");
#endif

    ESP_LOGI(TAG, "QSPI directo estilo Arduino: spi_mode=0 pclk=%d Hz", LCD_QSPI_CLOCK_HZ);

    ret = spi_bus_initialize(LCD_HOST, &buscfg, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "spi_bus_initialize: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = spi_bus_add_device(LCD_HOST, &devcfg, &lcd_spi);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "spi_bus_add_device LCD: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "SPI QSPI directo creado");

    if (lcd_framebuffer == NULL) {
        size_t framebuffer_bytes = (size_t)LCD_H_RES * (size_t)LCD_V_RES * sizeof(uint16_t);
        lcd_framebuffer = heap_caps_malloc(framebuffer_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (lcd_framebuffer == NULL) {
            lcd_framebuffer = heap_caps_malloc(framebuffer_bytes, MALLOC_CAP_8BIT);
        }
        if (lcd_framebuffer == NULL) {
            ESP_LOGE(TAG, "No hay memoria para framebuffer LCD (%u bytes)", (unsigned)framebuffer_bytes);
            return ESP_ERR_NO_MEM;
        }
        ESP_LOGI(TAG, "Framebuffer LCD listo: %u bytes", (unsigned)framebuffer_bytes);
    }

    ESP_RETURN_ON_ERROR(lcd_qspi_write_cmd(0x01, NULL, 0), TAG, "SWRESET fallo");
    vTaskDelay(pdMS_TO_TICKS(120));

    for (size_t i = 0; i < (sizeof(waveshare_qspi_init_cmds) / sizeof(waveshare_qspi_init_cmds[0])); i++) {
        const axs15231b_lcd_init_cmd_t *init_cmd = &waveshare_qspi_init_cmds[i];
        ESP_RETURN_ON_ERROR(
            lcd_qspi_write_cmd(init_cmd->cmd, init_cmd->data, init_cmd->data_bytes),
            TAG,
            "init cmd LCD fallo");
        if (init_cmd->delay_ms > 0) {
            vTaskDelay(pdMS_TO_TICKS(init_cmd->delay_ms));
        }
    }

    ESP_LOGI(TAG, "panel init QSPI directo OK");

    vTaskDelay(pdMS_TO_TICKS(100));
    gpio_set_level(LCD_BACKLIGHT_PIN, 1);

    lcd_disponible = true;

#if LCD_RUN_BOOT_DIAGNOSTICS
    ESP_ERROR_CHECK(lcd_control_run_diagnostics());
#endif
    if (LCD_BOOT_SCREEN_ENABLE && !lcd_diagnostico_fijo) {
        lcd_show_boot_screen();
    }

    ESP_LOGI(TAG, "LCD FINALIZADA");
    return ESP_OK;
}

esp_err_t lcd_control_init(void)
{
    esp_err_t ret = init_waveshare_lcd();
    if (ret != ESP_OK) {
        return ret;
    }

#if !LCD_TOUCH_ENABLE
    ESP_LOGW(TAG, "Touch deshabilitado temporalmente para prueba LCD azul.");
    return ESP_OK;
#else
    esp_err_t touch_ret = init_waveshare_touch();
    if (touch_ret != ESP_OK) {
        ESP_LOGW(TAG, "Touch no disponible (%s). Continuando sin touch.", esp_err_to_name(touch_ret));
    }

    return ESP_OK;
#endif
}

bool lcd_control_is_available(void)
{
    return lcd_disponible && lcd_spi != NULL;
}

esp_err_t lcd_control_run_diagnostics(void)
{
    if (!lcd_disponible || lcd_spi == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "LCD diagnostico fijo: fondo azul con texto");
    lcd_batch_mode = true;
    lcd_fill_rect(0, 0, LCD_H_RES, LCD_V_RES, LCD_COLOR_BLUE);
    lcd_draw_string(36, 34, "SOLEAC", LCD_COLOR_WHITE, LCD_COLOR_BLUE, 5);
    lcd_draw_string(36, 112, "LCD OK", LCD_COLOR_WHITE, LCD_COLOR_BLUE, 4);
    lcd_draw_string(36, 184, "ESP32-S3", LCD_COLOR_YELLOW, LCD_COLOR_BLUE, 3);
    lcd_draw_string(36, 244, "PSRAM OK", LCD_COLOR_GREEN, LCD_COLOR_BLUE, 3);
    lcd_batch_mode = false;
    ESP_RETURN_ON_ERROR(lcd_flush_framebuffer(), TAG, "flush diagnostico LCD fallo");
#if LCD_HOLD_DIAGNOSTICS
    lcd_diagnostico_fijo = true;
#endif
    vTaskDelay(pdMS_TO_TICKS(900));
    return ESP_OK;
}

typedef enum {
    LCD_SCREEN_NONE = 0,
    LCD_SCREEN_DEMO,
    LCD_SCREEN_MEMS_SIN_DATOS,
    LCD_SCREEN_CALIBRANDO,
    LCD_SCREEN_ALARMA,
    LCD_SCREEN_BUSCANDO,
} lcd_screen_id_t;

static lcd_screen_id_t lcd_pantalla_actual = LCD_SCREEN_NONE;

void lcd_control_render(uint32_t now_ms, const detector_state_t *state, bool alarma_ultra_activa)
{
#if !LCD_RUNTIME_RENDER_ENABLE
    (void)now_ms;
    (void)state;
    (void)alarma_ultra_activa;
    return;
#else
    char line[48];
    bool fondo_nuevo;
    bool debe_refrescar = false;
    float intensidad_visible;

    if (!lcd_disponible || lcd_spi == NULL) {
        return;
    }
    if (lcd_diagnostico_fijo) {
        return;
    }
    if ((now_ms - ultimo_refresco_lcd_ms) < LCD_REFRESH_MS) {
        return;
    }
    ultimo_refresco_lcd_ms = now_ms;
    lcd_batch_mode = true;

    if (state->modo_prueba_activo) {
        // Modo demo: el fondo y las etiquetas fijas solo se redibujan al entrar
        // a este modo; en refrescos siguientes solo se actualizan los valores.
        fondo_nuevo = (lcd_pantalla_actual != LCD_SCREEN_DEMO);
        lcd_pantalla_actual = LCD_SCREEN_DEMO;

        if (fondo_nuevo) {
            lcd_fill_rect(0, 0, LCD_H_RES, LCD_V_RES, LCD_COLOR_BLACK);
            lcd_draw_string(18, 16, "SOLEAC", LCD_COLOR_CYAN, LCD_COLOR_BLACK, 5);
            lcd_draw_string(20, 76, "DEMO", LCD_COLOR_YELLOW, LCD_COLOR_BLACK, 5);
        }

        snprintf(line, sizeof(line), "AUDIO:%ld", lroundf(state->nivel_audio));
        lcd_fill_rect(20, 150, 280, 26, LCD_COLOR_BLACK);
        lcd_draw_string(20, 150, line, LCD_COLOR_WHITE, LCD_COLOR_BLACK, 3);
        lcd_draw_meter(20, 184, 280, 24, state->nivel_audio, 30000.0f, LCD_COLOR_BLUE);

        snprintf(line, sizeof(line), "INT:%ld%%", lroundf(state->intensidad_pct));
        lcd_fill_rect(20, 228, 280, 26, LCD_COLOR_BLACK);
        lcd_draw_string(20, 228, line, LCD_COLOR_YELLOW, LCD_COLOR_BLACK, 3);
        lcd_draw_meter(
            20, 262, 280, 24,
            state->intensidad_pct,
            100.0f,
            LCD_COLOR_YELLOW);

        snprintf(line, sizeof(line), "FREQ:%ldHZ", lroundf(state->frecuencia_dominante_hz));
        lcd_fill_rect(20, 318, 280, 18, LCD_COLOR_BLACK);
        lcd_draw_string(20, 318, line, LCD_COLOR_GRAY, LCD_COLOR_BLACK, 2);

        snprintf(line, sizeof(line), "HF:%ld  U:%ld", lroundf(state->nivel_alta_frecuencia), lroundf(state->umbral_ultrasonido));
        lcd_fill_rect(20, 350, 280, 18, LCD_COLOR_BLACK);
        lcd_draw_string(20, 350, line, LCD_COLOR_GRAY, LCD_COLOR_BLACK, 2);

        snprintf(line, sizeof(line), "SIM:%s MQ9:%ld%%",
                 state->mq2_gas_detectado ? "GAS" : "SIN GAS",
                 lroundf(state->mq2_pct));
        lcd_fill_rect(20, 424, 280, 18, LCD_COLOR_BLACK);
        lcd_draw_string(20, 424, line, LCD_COLOR_GRAY, LCD_COLOR_BLACK, 2);
        debe_refrescar = true;
        goto flush_frame;
    }

    if (state->mems_capture_status != MEMS_CAPTURE_OK) {
        lcd_screen_id_t pantalla_mems =
            (state->mems_capture_status == MEMS_CAPTURE_STARTING)
                ? LCD_SCREEN_CALIBRANDO
                : LCD_SCREEN_MEMS_SIN_DATOS;
        fondo_nuevo = (lcd_pantalla_actual != pantalla_mems);
        lcd_pantalla_actual = pantalla_mems;

        if (fondo_nuevo) {
            lcd_fill_rect(0, 0, LCD_H_RES, LCD_V_RES, LCD_COLOR_RED);
            lcd_draw_string_centered(18, "SOLEAC", LCD_COLOR_WHITE, LCD_COLOR_RED, 2, false);
            if (state->mems_capture_status == MEMS_CAPTURE_STARTING) {
                lcd_draw_string_centered(150, "INICIANDO MEMS", LCD_COLOR_WHITE, LCD_COLOR_RED, 3, true);
            } else {
                lcd_draw_string_centered(120, "MEMS SIN DATOS", LCD_COLOR_WHITE, LCD_COLOR_RED, 3, true);
                lcd_draw_string_centered(190, "REVISAR CONEXION DE DATOS", LCD_COLOR_YELLOW, LCD_COLOR_RED, 2, false);
                lcd_draw_string_centered(220, "CLK / SELECT / VDD", LCD_COLOR_YELLOW, LCD_COLOR_RED, 2, false);
            }
        }

        snprintf(line, sizeof(line), "PDM:%s",
                 state->mems_capture_status == MEMS_CAPTURE_STARTING ? "ESPERANDO" : "NO VALIDO");
        lcd_fill_rect(20, 270, 280, 20, LCD_COLOR_RED);
        lcd_draw_string_centered(270, line, LCD_COLOR_WHITE, LCD_COLOR_RED, 2, false);
        lcd_draw_meter(20, 320, 280, 24, 0.0f, 100.0f, LCD_COLOR_YELLOW);
        snprintf(line, sizeof(line), "FFT:%s  INT:--", "SIN MUESTRAS");
        lcd_fill_rect(20, 370, 280, 18, LCD_COLOR_RED);
        lcd_draw_string_centered(370, line, LCD_COLOR_WHITE, LCD_COLOR_RED, 1, false);

        // El MQ-9 funciona de forma independiente al MEMS. Mantener su
        // estado visible en esta pantalla permite informar una deteccion de
        // gas aunque el audio/PDM no entregue muestras validas.
        snprintf(line, sizeof(line), "MQ9: %s %ld%%",
                 state->mq2_gas_detectado ? "GAS DETECTADO" : "SIN GAS",
                 lroundf(state->mq2_pct));
        lcd_fill_rect(20, 420, 280, 24, LCD_COLOR_RED);
        lcd_draw_string_centered(
            420,
            line,
            state->mq2_gas_detectado ? LCD_COLOR_YELLOW : LCD_COLOR_WHITE,
            LCD_COLOR_RED,
            2,
            state->mq2_gas_detectado);
        debe_refrescar = true;
        goto flush_frame;
    }

    if (!state->baseline_lista) {
        // Se sigue calibrando, pero ya mostramos el modo de busqueda principal.
        if (lcd_pantalla_actual != LCD_SCREEN_CALIBRANDO) {
            lcd_pantalla_actual = LCD_SCREEN_CALIBRANDO;
            lcd_fill_rect(0, 0, LCD_H_RES, LCD_V_RES, LCD_COLOR_WHITE);
            lcd_draw_string_centered(18, "SOLEAC", LCD_COLOR_GRAY, LCD_COLOR_WHITE, 2, false);
            lcd_draw_string_centered(72, "BUSCANDO", LCD_COLOR_BLACK, LCD_COLOR_WHITE, 4, true);
            lcd_draw_string_centered(120, "UNA FUGA", LCD_COLOR_BLACK, LCD_COLOR_WHITE, 4, true);
            lcd_draw_string_centered(400, "CALIBRANDO SENSOR", LCD_COLOR_GRAY, LCD_COLOR_WHITE, 2, false);
        }
        snprintf(line, sizeof(line), "MQ9:%s %ld%%",
                 state->mq2_conectado
                     ? (state->mq2_gas_detectado ? "GAS" : "SIN GAS")
                     : "DESC",
                 lroundf(state->mq2_pct));
        lcd_fill_rect(20, 434, 280, 18, LCD_COLOR_WHITE);
        lcd_draw_string(20, 434, line, LCD_COLOR_GRAY, LCD_COLOR_WHITE, 2);
        debe_refrescar = true;
        goto flush_frame;
    }

    if (alarma_ultra_activa) {
        fondo_nuevo = (lcd_pantalla_actual != LCD_SCREEN_ALARMA);
        lcd_pantalla_actual = LCD_SCREEN_ALARMA;

        if (fondo_nuevo) {
            lcd_fill_rect(0, 0, LCD_H_RES, LCD_V_RES, LCD_COLOR_RED);
            lcd_draw_string_centered(16, "SOLEAC", LCD_COLOR_WHITE, LCD_COLOR_RED, 2, false);
            lcd_draw_string_centered(
                60,
                state->mq2_gas_detectado ? "GAS CONFIRMADO" : "GAS NO CONFIRMADO",
                LCD_COLOR_WHITE,
                LCD_COLOR_RED,
                2,
                true);
        }

        snprintf(line, sizeof(line), "INTENSIDAD:%ld%%", lroundf(state->intensidad_pct));
        lcd_fill_rect(0, 120, LCD_H_RES, 20, LCD_COLOR_RED);
        lcd_draw_string_centered(120, line, LCD_COLOR_WHITE, LCD_COLOR_RED, 2, false);

        lcd_draw_string_centered(
            170,
            state->mq2_gas_detectado ? "FUGA DE GAS" : "ULTRASONIDO DETECTADO",
            LCD_COLOR_YELLOW,
            LCD_COLOR_RED,
            state->mq2_gas_detectado ? 3 : 2,
            true);

        lcd_draw_meter(
            20, 230, 280, 28,
            state->intensidad_pct,
            200.0f,
            LCD_COLOR_YELLOW);

        snprintf(line, sizeof(line), "FREQ:%ldHZ", lroundf(state->frecuencia_dominante_hz));
        lcd_fill_rect(0, 280, LCD_H_RES, 16, LCD_COLOR_RED);
        lcd_draw_string_centered(280, line, LCD_COLOR_WHITE, LCD_COLOR_RED, 1, false);

        snprintf(line, sizeof(line), "HF:%ld  U:%ld", lroundf(state->nivel_alta_frecuencia), lroundf(state->umbral_ultrasonido));
        lcd_fill_rect(0, 306, LCD_H_RES, 16, LCD_COLOR_RED);
        lcd_draw_string_centered(306, line, LCD_COLOR_WHITE, LCD_COLOR_RED, 1, false);

        snprintf(line, sizeof(line), "MQ9:%s %ld%%",
                 state->mq2_gas_detectado ? "GAS" : "SIN GAS",
                 lroundf(state->mq2_pct));
        lcd_fill_rect(0, 336, LCD_H_RES, 18, LCD_COLOR_RED);
        lcd_draw_string_centered(336, line, LCD_COLOR_WHITE, LCD_COLOR_RED, 2, false);

        snprintf(line, sizeof(line), "MA40:%ldMV", lroundf(state->ma40_mv));
        lcd_fill_rect(0, 366, LCD_H_RES, 16, LCD_COLOR_RED);
        lcd_draw_string_centered(366, line, LCD_COLOR_WHITE, LCD_COLOR_RED, 1, false);

        debe_refrescar = true;
        goto flush_frame;
    }

    // Todo configurado y sin alarma: buscando una fuga. El fondo y el titulo
    // solo se redibujan al entrar a este modo; los valores se actualizan siempre.
    fondo_nuevo = (lcd_pantalla_actual != LCD_SCREEN_BUSCANDO);
    lcd_pantalla_actual = LCD_SCREEN_BUSCANDO;

    if (fondo_nuevo) {
        lcd_fill_rect(0, 0, LCD_H_RES, LCD_V_RES, LCD_COLOR_WHITE);
        lcd_draw_string_centered(16, "SOLEAC", LCD_COLOR_GRAY, LCD_COLOR_WHITE, 2, false);
        lcd_draw_string_centered(70, "BUSCANDO", LCD_COLOR_BLACK, LCD_COLOR_WHITE, 4, true);
        lcd_draw_string_centered(70 + (LCD_FONT_HEIGHT * 4) + 12, "UNA FUGA", LCD_COLOR_BLACK, LCD_COLOR_WHITE, 4, true);
    }

    snprintf(line, sizeof(line), "AUDIO:%ld", lroundf(state->nivel_audio));
    lcd_fill_rect(20, 260, 280, 18, LCD_COLOR_WHITE);
    lcd_draw_string(20, 260, line, LCD_COLOR_GRAY, LCD_COLOR_WHITE, 2);
    lcd_draw_meter(20, 288, 280, 20, state->nivel_audio, 30000.0f, LCD_COLOR_BLUE);

    intensidad_visible = state->fft_detecta_ultrasonido ? state->intensidad_pct : 0.0f;
    snprintf(line, sizeof(line), "INT:%ld%%  F:%ldHZ", lroundf(intensidad_visible), lroundf(state->frecuencia_dominante_hz));
    lcd_fill_rect(20, 320, 280, 18, LCD_COLOR_WHITE);
    lcd_draw_string(20, 320, line, LCD_COLOR_GRAY, LCD_COLOR_WHITE, 2);
    // En reposo la intensidad representa ruido relativo, no una fuga.
    // Mostrar verde únicamente cuando el detector FFT haya superado el
    // umbral; de lo contrario la barra debe permanecer vacía.
    lcd_draw_meter(20, 348, 280, 20, intensidad_visible, 100.0f, LCD_COLOR_GREEN);

    snprintf(line, sizeof(line), "HF:%ld U:%ld", lroundf(state->nivel_alta_frecuencia), lroundf(state->umbral_ultrasonido));
    lcd_fill_rect(20, 382, 280, 16, LCD_COLOR_WHITE);
    lcd_draw_string(20, 382, line, LCD_COLOR_GRAY, LCD_COLOR_WHITE, 1);

    snprintf(line, sizeof(line), "MQ9:%s %ld%%",
             state->mq2_gas_detectado ? "GAS DETECTADO" : "SIN GAS",
             lroundf(state->mq2_pct));
    lcd_fill_rect(20, 424, 280, 18, LCD_COLOR_WHITE);
    lcd_draw_string(20, 424, line,
                    state->mq2_gas_detectado ? LCD_COLOR_YELLOW : LCD_COLOR_GRAY,
                    LCD_COLOR_WHITE, 2);

    snprintf(line, sizeof(line), "MA40:%ldMV", lroundf(state->ma40_mv));
    lcd_fill_rect(20, 450, 280, 16, LCD_COLOR_WHITE);
    lcd_draw_string(20, 450, line, LCD_COLOR_GRAY, LCD_COLOR_WHITE, 1);
    debe_refrescar = true;

flush_frame:
    lcd_batch_mode = false;
    if (debe_refrescar) {
        esp_err_t flush_ret = lcd_flush_framebuffer();
        if (flush_ret != ESP_OK) {
            ESP_LOGW(TAG, "No se pudo refrescar framebuffer LCD: %s", esp_err_to_name(flush_ret));
        }
    }
#endif
}

bool lcd_control_touch_demo_requested(uint32_t now_ms)
{
#if !TOUCH_DEMO_ENABLE
    (void)now_ms;
    return false;
#else
    if (!touch_disponible || touch_panel == NULL) {
        return false;
    }

    uint16_t touch_x[TOUCH_MAX_POINTS];
    uint16_t touch_y[TOUCH_MAX_POINTS];
    uint8_t touch_cnt = 0;

    esp_lcd_touch_read_data(touch_panel);
    bool touched = esp_lcd_touch_get_coordinates(
        touch_panel, touch_x, touch_y, NULL, &touch_cnt, TOUCH_MAX_POINTS);
    if (touched && touch_cnt > 0 && touch_y[0] >= TOUCH_DEMO_Y_MIN) {
        (void)touch_x;
        if ((now_ms - ultimo_touch_demo_ms) >= TOUCH_DEBOUNCE_MS) {
            ultimo_touch_demo_ms = now_ms;
            return true;
        }
    }
    return false;
#endif
}
