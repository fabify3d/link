#ifndef STATUS_LED_H
#define STATUS_LED_H

#include "led_strip.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <cstdint>
#include <string>


#define FAB_GREEN 0x24CA70
#define FAB_ORANGE 0xFF8C42
#define FAB_YELLOW 0xD7FC00
#define FAB_CORAL 0xFF6B6B
#define FAB_RED 0xDC362E
#define FAB_BLUE 0x56CFE1

/**
 * @file status_led.h
 * @brief Lightweight LED control interface for ESP32-S3 using Espressif's led_strip component.
 *
 * This class wraps the low-level led_strip API to provide:
 *  - Easy RGB or HEX color control
 *  - Adjustable brightness (intensity)
 *  - Built-in blinking modes (none, slow, normal, fast, or custom)
 *
 * Designed for ultra-low resource use and long-term reliability.
 */

static const char *TAG_LED = "StatusLED";

/**
 * @enum BlinkMode
 * @brief Predefined blinking rates in milliseconds.
 */
enum class BlinkMode
{
    NONE = 0,
    SLOW = 1000,
    NORMAL = 500,
    FAST = 200,
    CUSTOM = -1
};

/**
 * @class StatusLED
 * @brief Provides a simple interface to control a single WS2812-style LED via RMT.
 */
class StatusLED
{
private:
    led_strip_handle_t led_strip = nullptr;
    int gpio_num;
    float intensity = 0.15f; // Default 15% brightness
    BlinkMode blink_mode = BlinkMode::NONE;
    int custom_blink_ms = 0;
    bool led_on = true;
    esp_timer_handle_t blink_timer = nullptr;

    uint8_t r = 0, g = 0, b = 0;

    static void blink_timer_callback(void *arg)
    {
        auto *self = static_cast<StatusLED *>(arg);
        self->led_on = !self->led_on;
        if (self->led_on)
            self->applyColor();
        else
            self->clear();
    }

    void startBlinking(int period_ms)
    {
        if (blink_timer)
        {
            esp_timer_stop(blink_timer);
            esp_timer_delete(blink_timer);
            blink_timer = nullptr;
        }

        if (period_ms <= 0)
        {
            applyColor();
            return;
        }

        esp_timer_create_args_t timer_args = {
            .callback = &StatusLED::blink_timer_callback,
            .arg = this,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "led_blink_timer",
        };
        esp_timer_create(&timer_args, &blink_timer);
        esp_timer_start_periodic(blink_timer, period_ms * 1000ULL);
    }

    void applyColor()
    {
        if (!led_strip)
            return;
        uint8_t rr = static_cast<uint8_t>(r * intensity);
        uint8_t gg = static_cast<uint8_t>(g * intensity);
        uint8_t bb = static_cast<uint8_t>(b * intensity);
        led_strip_set_pixel(led_strip, 0, rr, gg, bb);
        led_strip_refresh(led_strip);
    }

    void clear()
    {
        if (!led_strip)
            return;
        led_strip_clear(led_strip);
    }

public:
    StatusLED(int gpio = 48, int num_leds = 1) : gpio_num(gpio)
    {
        led_strip_config_t strip_config = {
            .strip_gpio_num = gpio_num,
            .max_leds = static_cast<uint32_t>(num_leds),
            .led_model = LED_MODEL_WS2812,
            .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
            .flags = {.invert_out = false}};

        led_strip_rmt_config_t rmt_config = {
            .clk_src = RMT_CLK_SRC_DEFAULT,
            .resolution_hz = 10 * 1000 * 1000, // 10 MHz
            .mem_block_symbols = 0,
            .flags = {.with_dma = 1}};

        ESP_ERROR_CHECK(led_strip_new_rmt_device(&strip_config, &rmt_config, &led_strip));
        ESP_LOGI(TAG_LED, "LED initialized on GPIO %d", gpio_num);
    }

    ~StatusLED()
    {
        if (blink_timer)
        {
            esp_timer_stop(blink_timer);
            esp_timer_delete(blink_timer);
        }
        if (led_strip)
        {
            led_strip_clear(led_strip);
        }
    }

    void setColor(uint8_t red, uint8_t green, uint8_t blue)
    {
        r = red;
        g = green;
        b = blue;
        applyColor();
    }

    void setColorHex(uint32_t hex)
    {
        uint8_t red = (hex >> 16) & 0xFF;
        uint8_t green = (hex >> 8) & 0xFF;
        uint8_t blue = hex & 0xFF;
        setColor(red, green, blue);
    }

    void setIntensity(float level)
    {
        if (level < 0.0f)
            level = 0.0f;
        if (level > 1.0f)
            level = 1.0f;
        intensity = level;
        applyColor();
    }

    void setBlinkingMode(BlinkMode mode, int custom_ms = 0)
    {
        blink_mode = mode;
        if (mode == BlinkMode::CUSTOM)
            custom_blink_ms = custom_ms;

        int period = 0;
        switch (mode)
        {
        case BlinkMode::NONE:
            period = 0;
            break;
        case BlinkMode::SLOW:
            period = 1000;
            break;
        case BlinkMode::NORMAL:
            period = 500;
            break;
        case BlinkMode::FAST:
            period = 200;
            break;
        case BlinkMode::CUSTOM:
            period = custom_ms;
            break;
        }

        startBlinking(period);
    }
};

#endif // STATUS_LED_H
