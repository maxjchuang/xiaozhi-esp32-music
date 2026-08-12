#include "wifi_board.h"
#include "codecs/box_audio_codec.h"
#include "display/lcd_display.h"
#include "application.h"
#include "assets/lang_config.h"
#include "button.h"
#include "config.h"
#include "backlight.h"
#include "emote_display.h"

#include <wifi_station.h>
#include <esp_log.h>
#include <esp_sleep.h>

#include <algorithm>
#include <atomic>

#include <driver/i2c_master.h>
#include <driver/i2c.h>
#include "i2c_device.h"
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_st77916.h>
#include "esp_lcd_touch_cst816s.h"
#include "touch.h"

#include "driver/temperature_sensor.h"
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#define TAG "EchoEar"

#define USE_LVGL_DEFAULT    0

LV_FONT_DECLARE(font_puhui_20_4);
LV_FONT_DECLARE(font_awesome_20_4);
temperature_sensor_handle_t temp_sensor = NULL;
static const st77916_lcd_init_cmd_t vendor_specific_init_yysj[] = {
    {0xF0, (uint8_t []){0x28}, 1, 0},
    {0xF2, (uint8_t []){0x28}, 1, 0},
    {0x73, (uint8_t []){0xF0}, 1, 0},
    {0x7C, (uint8_t []){0xD1}, 1, 0},
    {0x83, (uint8_t []){0xE0}, 1, 0},
    {0x84, (uint8_t []){0x61}, 1, 0},
    {0xF2, (uint8_t []){0x82}, 1, 0},
    {0xF0, (uint8_t []){0x00}, 1, 0},
    {0xF0, (uint8_t []){0x01}, 1, 0},
    {0xF1, (uint8_t []){0x01}, 1, 0},
    {0xB0, (uint8_t []){0x56}, 1, 0},
    {0xB1, (uint8_t []){0x4D}, 1, 0},
    {0xB2, (uint8_t []){0x24}, 1, 0},
    {0xB4, (uint8_t []){0x87}, 1, 0},
    {0xB5, (uint8_t []){0x44}, 1, 0},
    {0xB6, (uint8_t []){0x8B}, 1, 0},
    {0xB7, (uint8_t []){0x40}, 1, 0},
    {0xB8, (uint8_t []){0x86}, 1, 0},
    {0xBA, (uint8_t []){0x00}, 1, 0},
    {0xBB, (uint8_t []){0x08}, 1, 0},
    {0xBC, (uint8_t []){0x08}, 1, 0},
    {0xBD, (uint8_t []){0x00}, 1, 0},
    {0xC0, (uint8_t []){0x80}, 1, 0},
    {0xC1, (uint8_t []){0x10}, 1, 0},
    {0xC2, (uint8_t []){0x37}, 1, 0},
    {0xC3, (uint8_t []){0x80}, 1, 0},
    {0xC4, (uint8_t []){0x10}, 1, 0},
    {0xC5, (uint8_t []){0x37}, 1, 0},
    {0xC6, (uint8_t []){0xA9}, 1, 0},
    {0xC7, (uint8_t []){0x41}, 1, 0},
    {0xC8, (uint8_t []){0x01}, 1, 0},
    {0xC9, (uint8_t []){0xA9}, 1, 0},
    {0xCA, (uint8_t []){0x41}, 1, 0},
    {0xCB, (uint8_t []){0x01}, 1, 0},
    {0xD0, (uint8_t []){0x91}, 1, 0},
    {0xD1, (uint8_t []){0x68}, 1, 0},
    {0xD2, (uint8_t []){0x68}, 1, 0},
    {0xF5, (uint8_t []){0x00, 0xA5}, 2, 0},
    {0xDD, (uint8_t []){0x4F}, 1, 0},
    {0xDE, (uint8_t []){0x4F}, 1, 0},
    {0xF1, (uint8_t []){0x10}, 1, 0},
    {0xF0, (uint8_t []){0x00}, 1, 0},
    {0xF0, (uint8_t []){0x02}, 1, 0},
    {0xE0, (uint8_t []){0xF0, 0x0A, 0x10, 0x09, 0x09, 0x36, 0x35, 0x33, 0x4A, 0x29, 0x15, 0x15, 0x2E, 0x34}, 14, 0},
    {0xE1, (uint8_t []){0xF0, 0x0A, 0x0F, 0x08, 0x08, 0x05, 0x34, 0x33, 0x4A, 0x39, 0x15, 0x15, 0x2D, 0x33}, 14, 0},
    {0xF0, (uint8_t []){0x10}, 1, 0},
    {0xF3, (uint8_t []){0x10}, 1, 0},
    {0xE0, (uint8_t []){0x07}, 1, 0},
    {0xE1, (uint8_t []){0x00}, 1, 0},
    {0xE2, (uint8_t []){0x00}, 1, 0},
    {0xE3, (uint8_t []){0x00}, 1, 0},
    {0xE4, (uint8_t []){0xE0}, 1, 0},
    {0xE5, (uint8_t []){0x06}, 1, 0},
    {0xE6, (uint8_t []){0x21}, 1, 0},
    {0xE7, (uint8_t []){0x01}, 1, 0},
    {0xE8, (uint8_t []){0x05}, 1, 0},
    {0xE9, (uint8_t []){0x02}, 1, 0},
    {0xEA, (uint8_t []){0xDA}, 1, 0},
    {0xEB, (uint8_t []){0x00}, 1, 0},
    {0xEC, (uint8_t []){0x00}, 1, 0},
    {0xED, (uint8_t []){0x0F}, 1, 0},
    {0xEE, (uint8_t []){0x00}, 1, 0},
    {0xEF, (uint8_t []){0x00}, 1, 0},
    {0xF8, (uint8_t []){0x00}, 1, 0},
    {0xF9, (uint8_t []){0x00}, 1, 0},
    {0xFA, (uint8_t []){0x00}, 1, 0},
    {0xFB, (uint8_t []){0x00}, 1, 0},
    {0xFC, (uint8_t []){0x00}, 1, 0},
    {0xFD, (uint8_t []){0x00}, 1, 0},
    {0xFE, (uint8_t []){0x00}, 1, 0},
    {0xFF, (uint8_t []){0x00}, 1, 0},
    {0x60, (uint8_t []){0x40}, 1, 0},
    {0x61, (uint8_t []){0x04}, 1, 0},
    {0x62, (uint8_t []){0x00}, 1, 0},
    {0x63, (uint8_t []){0x42}, 1, 0},
    {0x64, (uint8_t []){0xD9}, 1, 0},
    {0x65, (uint8_t []){0x00}, 1, 0},
    {0x66, (uint8_t []){0x00}, 1, 0},
    {0x67, (uint8_t []){0x00}, 1, 0},
    {0x68, (uint8_t []){0x00}, 1, 0},
    {0x69, (uint8_t []){0x00}, 1, 0},
    {0x6A, (uint8_t []){0x00}, 1, 0},
    {0x6B, (uint8_t []){0x00}, 1, 0},
    {0x70, (uint8_t []){0x40}, 1, 0},
    {0x71, (uint8_t []){0x03}, 1, 0},
    {0x72, (uint8_t []){0x00}, 1, 0},
    {0x73, (uint8_t []){0x42}, 1, 0},
    {0x74, (uint8_t []){0xD8}, 1, 0},
    {0x75, (uint8_t []){0x00}, 1, 0},
    {0x76, (uint8_t []){0x00}, 1, 0},
    {0x77, (uint8_t []){0x00}, 1, 0},
    {0x78, (uint8_t []){0x00}, 1, 0},
    {0x79, (uint8_t []){0x00}, 1, 0},
    {0x7A, (uint8_t []){0x00}, 1, 0},
    {0x7B, (uint8_t []){0x00}, 1, 0},
    {0x80, (uint8_t []){0x48}, 1, 0},
    {0x81, (uint8_t []){0x00}, 1, 0},
    {0x82, (uint8_t []){0x06}, 1, 0},
    {0x83, (uint8_t []){0x02}, 1, 0},
    {0x84, (uint8_t []){0xD6}, 1, 0},
    {0x85, (uint8_t []){0x04}, 1, 0},
    {0x86, (uint8_t []){0x00}, 1, 0},
    {0x87, (uint8_t []){0x00}, 1, 0},
    {0x88, (uint8_t []){0x48}, 1, 0},
    {0x89, (uint8_t []){0x00}, 1, 0},
    {0x8A, (uint8_t []){0x08}, 1, 0},
    {0x8B, (uint8_t []){0x02}, 1, 0},
    {0x8C, (uint8_t []){0xD8}, 1, 0},
    {0x8D, (uint8_t []){0x04}, 1, 0},
    {0x8E, (uint8_t []){0x00}, 1, 0},
    {0x8F, (uint8_t []){0x00}, 1, 0},
    {0x90, (uint8_t []){0x48}, 1, 0},
    {0x91, (uint8_t []){0x00}, 1, 0},
    {0x92, (uint8_t []){0x0A}, 1, 0},
    {0x93, (uint8_t []){0x02}, 1, 0},
    {0x94, (uint8_t []){0xDA}, 1, 0},
    {0x95, (uint8_t []){0x04}, 1, 0},
    {0x96, (uint8_t []){0x00}, 1, 0},
    {0x97, (uint8_t []){0x00}, 1, 0},
    {0x98, (uint8_t []){0x48}, 1, 0},
    {0x99, (uint8_t []){0x00}, 1, 0},
    {0x9A, (uint8_t []){0x0C}, 1, 0},
    {0x9B, (uint8_t []){0x02}, 1, 0},
    {0x9C, (uint8_t []){0xDC}, 1, 0},
    {0x9D, (uint8_t []){0x04}, 1, 0},
    {0x9E, (uint8_t []){0x00}, 1, 0},
    {0x9F, (uint8_t []){0x00}, 1, 0},
    {0xA0, (uint8_t []){0x48}, 1, 0},
    {0xA1, (uint8_t []){0x00}, 1, 0},
    {0xA2, (uint8_t []){0x05}, 1, 0},
    {0xA3, (uint8_t []){0x02}, 1, 0},
    {0xA4, (uint8_t []){0xD5}, 1, 0},
    {0xA5, (uint8_t []){0x04}, 1, 0},
    {0xA6, (uint8_t []){0x00}, 1, 0},
    {0xA7, (uint8_t []){0x00}, 1, 0},
    {0xA8, (uint8_t []){0x48}, 1, 0},
    {0xA9, (uint8_t []){0x00}, 1, 0},
    {0xAA, (uint8_t []){0x07}, 1, 0},
    {0xAB, (uint8_t []){0x02}, 1, 0},
    {0xAC, (uint8_t []){0xD7}, 1, 0},
    {0xAD, (uint8_t []){0x04}, 1, 0},
    {0xAE, (uint8_t []){0x00}, 1, 0},
    {0xAF, (uint8_t []){0x00}, 1, 0},
    {0xB0, (uint8_t []){0x48}, 1, 0},
    {0xB1, (uint8_t []){0x00}, 1, 0},
    {0xB2, (uint8_t []){0x09}, 1, 0},
    {0xB3, (uint8_t []){0x02}, 1, 0},
    {0xB4, (uint8_t []){0xD9}, 1, 0},
    {0xB5, (uint8_t []){0x04}, 1, 0},
    {0xB6, (uint8_t []){0x00}, 1, 0},
    {0xB7, (uint8_t []){0x00}, 1, 0},
    {0xB8, (uint8_t []){0x48}, 1, 0},
    {0xB9, (uint8_t []){0x00}, 1, 0},
    {0xBA, (uint8_t []){0x0B}, 1, 0},
    {0xBB, (uint8_t []){0x02}, 1, 0},
    {0xBC, (uint8_t []){0xDB}, 1, 0},
    {0xBD, (uint8_t []){0x04}, 1, 0},
    {0xBE, (uint8_t []){0x00}, 1, 0},
    {0xBF, (uint8_t []){0x00}, 1, 0},
    {0xC0, (uint8_t []){0x10}, 1, 0},
    {0xC1, (uint8_t []){0x47}, 1, 0},
    {0xC2, (uint8_t []){0x56}, 1, 0},
    {0xC3, (uint8_t []){0x65}, 1, 0},
    {0xC4, (uint8_t []){0x74}, 1, 0},
    {0xC5, (uint8_t []){0x88}, 1, 0},
    {0xC6, (uint8_t []){0x99}, 1, 0},
    {0xC7, (uint8_t []){0x01}, 1, 0},
    {0xC8, (uint8_t []){0xBB}, 1, 0},
    {0xC9, (uint8_t []){0xAA}, 1, 0},
    {0xD0, (uint8_t []){0x10}, 1, 0},
    {0xD1, (uint8_t []){0x47}, 1, 0},
    {0xD2, (uint8_t []){0x56}, 1, 0},
    {0xD3, (uint8_t []){0x65}, 1, 0},
    {0xD4, (uint8_t []){0x74}, 1, 0},
    {0xD5, (uint8_t []){0x88}, 1, 0},
    {0xD6, (uint8_t []){0x99}, 1, 0},
    {0xD7, (uint8_t []){0x01}, 1, 0},
    {0xD8, (uint8_t []){0xBB}, 1, 0},
    {0xD9, (uint8_t []){0xAA}, 1, 0},
    {0xF3, (uint8_t []){0x01}, 1, 0},
    {0xF0, (uint8_t []){0x00}, 1, 0},
    {0x21, (uint8_t []){}, 0, 0},
    {0x11, (uint8_t []){}, 0, 0},
    {0x00, (uint8_t []){}, 0, 120},
};
float tsens_value;
gpio_num_t AUDIO_I2S_GPIO_DIN = AUDIO_I2S_GPIO_DIN_1;
gpio_num_t AUDIO_CODEC_PA_PIN = AUDIO_CODEC_PA_PIN_1;
gpio_num_t QSPI_PIN_NUM_LCD_RST = QSPI_PIN_NUM_LCD_RST_1;
gpio_num_t TOUCH_PAD2 = TOUCH_PAD2_1;
gpio_num_t UART1_TX = UART1_TX_1;
gpio_num_t UART1_RX = UART1_RX_1;

class Charge : public I2cDevice {
public:
    Charge(i2c_master_bus_handle_t i2c_bus, uint8_t addr) : I2cDevice(i2c_bus, addr)
    {
    }

    void SampleAndApplyPolicy()
    {
        uint8_t buffer[8] = {};
        ReadRegs(0x08, buffer, 6);
        uint8_t soc_buffer[2] = {};
        ReadRegs(0x2c, soc_buffer, sizeof(soc_buffer));
        ESP_ERROR_CHECK(temperature_sensor_get_celsius(temp_sensor, &tsens_value));

        const uint16_t voltage_mv = static_cast<uint16_t>(buffer[1] << 8 | buffer[0]);
        const uint16_t battery_status = static_cast<uint16_t>(buffer[3] << 8 | buffer[2]);
        const int16_t current_ma = static_cast<int16_t>(buffer[5] << 8 | buffer[4]);
        const uint16_t soc = static_cast<uint16_t>(soc_buffer[1] << 8 | soc_buffer[0]);

        if (voltage_mv < 2500 || voltage_mv > 5000 || soc > 100) {
            valid_.store(false);
            ESP_LOGW(TAG, "Battery gauge returned invalid data: soc=%u voltage=%umV status=0x%04x",
                     soc, voltage_mv, battery_status);
            return;
        }

        // BQ27220 BatteryStatus bit 0 (DSG) is set while the cell is supplying
        // the system.  A clear DSG bit means the device is externally powered
        // (charging or relaxed), which suppresses every low-battery action.
        const bool discharging = (battery_status & 0x0001) != 0;
        level_.store(static_cast<int>(soc));
        discharging_.store(discharging);
        charging_.store(!discharging);
        valid_.store(true);

        ESP_LOGI(TAG, "Battery: soc=%u%% voltage=%umV current=%dmA status=0x%04x external_power=%s",
                 soc, voltage_mv, current_ma, battery_status, discharging ? "no" : "yes");

        if (!discharging) {
            ResetDischargeCycle();
            return;
        }

        UpdateCounter(low_20_samples_, soc <= 20);
        UpdateCounter(low_10_samples_, soc <= 10);
        UpdateCounter(shutdown_samples_, soc < 5);

        if (shutdown_samples_ >= kStableSampleCount && !shutdown_requested_) {
            shutdown_requested_ = true;
            ESP_LOGW(TAG, "Battery below 5%% for %d samples; entering low-power shutdown",
                     kStableSampleCount);
            Application::GetInstance().Schedule([]() {
                Board::GetInstance().GetDisplay()->SetBehavior({
                    DisplayBehavior::kFatalError,
                    DisplayBehaviorSource::kSystem,
                    "电量过低，即将关机",
                    2500,
                });
            });
            return;
        }

        if (low_10_samples_ >= kStableSampleCount && !notified_10_) {
            notified_10_ = true;
            notified_20_ = true;
            PostLowBatteryReminder(10, "电量剩余 10%，请尽快充电");
        } else if (low_20_samples_ >= kStableSampleCount && !notified_20_) {
            notified_20_ = true;
            PostLowBatteryReminder(20, "电量剩余 20%，请充电");
        }
    }

    static void TaskFunction(void *pvParameters)
    {
        Charge* charge = static_cast<Charge*>(pvParameters);
        while (true) {
            charge->SampleAndApplyPolicy();
            if (charge->shutdown_requested_) {
                vTaskDelay(pdMS_TO_TICKS(3000));
                // Re-sample so plugging the cable in during the warning cancels
                // shutdown instead of putting a powered device to sleep.
                charge->SampleAndApplyPolicy();
                if (charge->valid_.load() && charge->discharging_.load() &&
                    charge->level_.load() < 5) {
                    charge->EnterLowPowerShutdown();
                }
                charge->shutdown_requested_ = false;
            }
            vTaskDelay(pdMS_TO_TICKS(kSampleIntervalMs));
        }
    }

    bool GetBatteryLevel(int& level, bool& charging, bool& discharging) const
    {
        if (!valid_.load()) {
            return false;
        }
        level = level_.load();
        charging = charging_.load();
        discharging = discharging_.load();
        return true;
    }

private:
    static constexpr int kSampleIntervalMs = 5000;
    static constexpr int kStableSampleCount = 3;

    static void UpdateCounter(int& counter, bool condition)
    {
        counter = condition ? std::min(counter + 1, kStableSampleCount) : 0;
    }

    void ResetDischargeCycle()
    {
        if (notified_20_ || notified_10_ || shutdown_requested_) {
            ESP_LOGI(TAG, "External power detected; reset low-battery notification cycle");
        }
        notified_20_ = false;
        notified_10_ = false;
        shutdown_requested_ = false;
        low_20_samples_ = 0;
        low_10_samples_ = 0;
        shutdown_samples_ = 0;
    }

    static void PostLowBatteryReminder(int threshold, const char* detail)
    {
        ESP_LOGW(TAG, "Low-battery reminder at %d%%", threshold);
        const std::string message(detail);
        Application::GetInstance().Schedule([message]() {
            auto& app = Application::GetInstance();
            Board::GetInstance().GetDisplay()->SetBehavior({
                DisplayBehavior::kRecoverableError,
                DisplayBehaviorSource::kSystem,
                message,
                4500,
            });
            app.PlaySound(Lang::Sounds::P3_LOW_BATTERY);
        });
    }

    [[noreturn]] static void EnterLowPowerShutdown()
    {
        ESP_LOGW(TAG, "Low battery: disabling peripherals and entering deep sleep");
        if (auto* backlight = Board::GetInstance().GetBacklight()) {
            backlight->SetBrightness(0);
        }
        // POWER_CTRL only gates LCD/SD power on EchoEar. The SAM8108 power
        // latch is controlled by the physical button, so deep sleep is the
        // closest firmware-controlled equivalent to a full shutdown.
        gpio_set_level(POWER_CTRL, 1);
        vTaskDelay(pdMS_TO_TICKS(100));
        esp_deep_sleep_start();
        __builtin_unreachable();
    }

    std::atomic<bool> valid_{false};
    std::atomic<int> level_{-1};
    std::atomic<bool> charging_{false};
    std::atomic<bool> discharging_{false};
    int low_20_samples_ = 0;
    int low_10_samples_ = 0;
    int shutdown_samples_ = 0;
    bool notified_20_ = false;
    bool notified_10_ = false;
    bool shutdown_requested_ = false;
};

class Cst816s : public I2cDevice {
public:
    struct TouchPoint_t {
        int num = 0;
        int x = -1;
        int y = -1;
    };

    enum TouchEvent {
        TOUCH_NONE,
        TOUCH_PRESS,
        TOUCH_RELEASE,
        TOUCH_HOLD
    };

    Cst816s(i2c_master_bus_handle_t i2c_bus, uint8_t addr) : I2cDevice(i2c_bus, addr)
    {
        read_buffer_ = new uint8_t[6];
        was_touched_ = false;
        press_count_ = 0;

        // Create touch interrupt semaphore
        touch_isr_mux_ = xSemaphoreCreateBinary();
        if (touch_isr_mux_ == NULL) {
            ESP_LOGE("EchoEar", "Failed to create touch semaphore");
        }
    }

    ~Cst816s()
    {
        delete[] read_buffer_;

        // Delete semaphore if it exists
        if (touch_isr_mux_ != NULL) {
            vSemaphoreDelete(touch_isr_mux_);
            touch_isr_mux_ = NULL;
        }
    }

    void UpdateTouchPoint()
    {
        ReadRegs(0x02, read_buffer_, 6);
        tp_.num = read_buffer_[0] & 0x0F;
        tp_.x = ((read_buffer_[1] & 0x0F) << 8) | read_buffer_[2];
        tp_.y = ((read_buffer_[3] & 0x0F) << 8) | read_buffer_[4];
    }

    const TouchPoint_t &GetTouchPoint()
    {
        return tp_;
    }

    TouchEvent CheckTouchEvent()
    {
        bool is_touched = (tp_.num > 0);
        TouchEvent event = TOUCH_NONE;

        if (is_touched && !was_touched_) {
            // Press event (transition from not touched to touched)
            press_count_++;
            event = TOUCH_PRESS;
            ESP_LOGI("EchoEar", "TOUCH PRESS - count: %d, x: %d, y: %d", press_count_, tp_.x, tp_.y);
        } else if (!is_touched && was_touched_) {
            // Release event (transition from touched to not touched)
            event = TOUCH_RELEASE;
            ESP_LOGI("EchoEar", "TOUCH RELEASE - total presses: %d", press_count_);
        } else if (is_touched && was_touched_) {
            // Continuous touch (hold)
            event = TOUCH_HOLD;
            ESP_LOGD("EchoEar", "TOUCH HOLD - x: %d, y: %d", tp_.x, tp_.y);
        }

        // Update previous state
        was_touched_ = is_touched;
        return event;
    }

    int GetPressCount() const
    {
        return press_count_;
    }

    void ResetPressCount()
    {
        press_count_ = 0;
    }

    // Semaphore management methods
    SemaphoreHandle_t GetTouchSemaphore()
    {
        return touch_isr_mux_;
    }

    bool WaitForTouchEvent(TickType_t timeout = portMAX_DELAY)
    {
        if (touch_isr_mux_ != NULL) {
            return xSemaphoreTake(touch_isr_mux_, timeout) == pdTRUE;
        }
        return false;
    }

    void NotifyTouchEvent()
    {
        if (touch_isr_mux_ != NULL) {
            BaseType_t xHigherPriorityTaskWoken = pdFALSE;
            xSemaphoreGiveFromISR(touch_isr_mux_, &xHigherPriorityTaskWoken);
            portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
        }
    }

private:
    uint8_t* read_buffer_ = nullptr;
    TouchPoint_t tp_;

    // Touch state tracking
    bool was_touched_;
    int press_count_;

    // Touch interrupt semaphore
    SemaphoreHandle_t touch_isr_mux_;
};

class EspS3Cat : public WifiBoard {
private:
    i2c_master_bus_handle_t i2c_bus_;
    Cst816s* cst816s_;
    Charge* charge_;
    Button boot_button_;
#if USE_LVGL_DEFAULT
    LcdDisplay* display_;
#else
    anim::EmoteDisplay* display_ = nullptr;
#endif
    PwmBacklight* backlight_ = nullptr;
    esp_timer_handle_t touchpad_timer_;
    esp_lcd_touch_handle_t tp;   // LCD touch handle

    void InitializeI2c()
    {
        i2c_master_bus_config_t i2c_bus_cfg = {
            .i2c_port = I2C_NUM_0,
            .sda_io_num = AUDIO_CODEC_I2C_SDA_PIN,
            .scl_io_num = AUDIO_CODEC_I2C_SCL_PIN,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .intr_priority = 0,
            .trans_queue_depth = 0,
            .flags = {
                .enable_internal_pullup = 1,
            },
        };
        ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_cfg, &i2c_bus_));

        temperature_sensor_config_t temp_sensor_config = TEMPERATURE_SENSOR_CONFIG_DEFAULT(10, 50);
        ESP_ERROR_CHECK(temperature_sensor_install(&temp_sensor_config, &temp_sensor));
        ESP_ERROR_CHECK(temperature_sensor_enable(temp_sensor));

    }
    uint8_t DetectPcbVersion()
    {
        esp_err_t ret = i2c_master_probe(i2c_bus_, 0x18, 100);
        uint8_t pcb_verison = 0;
        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "PCB verison V1.0");
            pcb_verison = 0;
        } else {
            gpio_config_t gpio_conf = {
                .pin_bit_mask = (1ULL << GPIO_NUM_48),
                .mode = GPIO_MODE_OUTPUT,
                .pull_up_en = GPIO_PULLUP_DISABLE,
                .pull_down_en = GPIO_PULLDOWN_DISABLE,
                .intr_type = GPIO_INTR_DISABLE
            };
            ESP_ERROR_CHECK(gpio_config(&gpio_conf));
            ESP_ERROR_CHECK(gpio_set_level(GPIO_NUM_48, 1));
            vTaskDelay(pdMS_TO_TICKS(100));
            ret = i2c_master_probe(i2c_bus_, 0x18, 100);
            if (ret == ESP_OK) {
                ESP_LOGI(TAG, "PCB verison V1.2");
                pcb_verison = 1;
                AUDIO_I2S_GPIO_DIN = AUDIO_I2S_GPIO_DIN_2;
                AUDIO_CODEC_PA_PIN = AUDIO_CODEC_PA_PIN_2;
                QSPI_PIN_NUM_LCD_RST = QSPI_PIN_NUM_LCD_RST_2;
                TOUCH_PAD2 = TOUCH_PAD2_2;
                UART1_TX = UART1_TX_2;
                UART1_RX = UART1_RX_2;
            } else {
                ESP_LOGE(TAG, "PCB version detection error");

            }
        }
        return pcb_verison;
    }

    static void touch_isr_callback(void* arg)
    {
        Cst816s* touchpad = static_cast<Cst816s*>(arg);
        if (touchpad != nullptr) {
            touchpad->NotifyTouchEvent();
        }
    }

    static void touch_event_task(void* arg)
    {
        Cst816s* touchpad = static_cast<Cst816s*>(arg);
        if (touchpad == nullptr) {
            ESP_LOGE(TAG, "Invalid touchpad pointer in touch_event_task");
            vTaskDelete(NULL);
            return;
        }

        while (true) {
            if (touchpad->WaitForTouchEvent()) {
                auto &app = Application::GetInstance();
                auto &board = (EspS3Cat &)Board::GetInstance();

                ESP_LOGI(TAG, "Touch event, TP_PIN_NUM_INT: %d", gpio_get_level(TP_PIN_NUM_INT));
                touchpad->UpdateTouchPoint();
                auto touch_event = touchpad->CheckTouchEvent();

                if (touch_event == Cst816s::TOUCH_RELEASE) {
                    if (app.GetDeviceState() == kDeviceStateStarting &&
                            !WifiStation::GetInstance().IsConnected()) {
                        board.ResetWifiConfiguration();
                    } else {
                        app.ToggleChatState();
                    }
                }
            }
        }
    }

    void InitializeCharge()
    {
        charge_ = new Charge(i2c_bus_, 0x55);
        xTaskCreatePinnedToCore(Charge::TaskFunction, "batterydecTask", 3 * 1024, charge_, 6, NULL, 0);
    }

    void InitializeCst816sTouchPad()
    {
        cst816s_ = new Cst816s(i2c_bus_, 0x15);

        xTaskCreatePinnedToCore(touch_event_task, "touch_task", 4 * 1024, cst816s_, 5, NULL, 1);

        const gpio_config_t int_gpio_config = {
            .pin_bit_mask = (1ULL << TP_PIN_NUM_INT),
            .mode = GPIO_MODE_INPUT,
            // .intr_type = GPIO_INTR_NEGEDGE
            .intr_type = GPIO_INTR_ANYEDGE
        };
        gpio_config(&int_gpio_config);
        gpio_install_isr_service(0);
        gpio_intr_enable(TP_PIN_NUM_INT);
        gpio_isr_handler_add(TP_PIN_NUM_INT, EspS3Cat::touch_isr_callback, cst816s_);
    }

    void InitializeSpi()
    {
        const spi_bus_config_t bus_config = TAIJIPI_ST77916_PANEL_BUS_QSPI_CONFIG(QSPI_PIN_NUM_LCD_PCLK,
                                                                                  QSPI_PIN_NUM_LCD_DATA0,
                                                                                  QSPI_PIN_NUM_LCD_DATA1,
                                                                                  QSPI_PIN_NUM_LCD_DATA2,
                                                                                  QSPI_PIN_NUM_LCD_DATA3,
                                                                                  QSPI_LCD_H_RES * 80 * sizeof(uint16_t));
        ESP_ERROR_CHECK(spi_bus_initialize(QSPI_LCD_HOST, &bus_config, SPI_DMA_CH_AUTO));
    }

    void Initializest77916Display(uint8_t pcb_verison)
    {

        esp_lcd_panel_io_handle_t panel_io = nullptr;
        esp_lcd_panel_handle_t panel = nullptr;

        const esp_lcd_panel_io_spi_config_t io_config = ST77916_PANEL_IO_QSPI_CONFIG(QSPI_PIN_NUM_LCD_CS, NULL, NULL);
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)QSPI_LCD_HOST, &io_config, &panel_io));
        st77916_vendor_config_t vendor_config = {
            .init_cmds = vendor_specific_init_yysj,
            .init_cmds_size = sizeof(vendor_specific_init_yysj) / sizeof(st77916_lcd_init_cmd_t),
            .flags = {
                .use_qspi_interface = 1,
            },
        };
        const esp_lcd_panel_dev_config_t panel_config = {
            .reset_gpio_num = QSPI_PIN_NUM_LCD_RST,
            .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
            .bits_per_pixel = QSPI_LCD_BIT_PER_PIXEL,
            .flags = {
                .reset_active_high = pcb_verison,
            },
            .vendor_config = &vendor_config,
        };
        ESP_ERROR_CHECK(esp_lcd_new_panel_st77916(panel_io, &panel_config, &panel));

        esp_lcd_panel_reset(panel);
        esp_lcd_panel_init(panel);
        esp_lcd_panel_disp_on_off(panel, true);
        esp_lcd_panel_swap_xy(panel, DISPLAY_SWAP_XY);
        esp_lcd_panel_mirror(panel, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y);

#if USE_LVGL_DEFAULT
        display_ = new SpiLcdDisplay(panel_io, panel,
        DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY, {
            .text_font = &font_puhui_20_4,
            .icon_font = &font_awesome_20_4,
            .emoji_font = font_emoji_64_init(),
        });
#else
        display_ = new anim::EmoteDisplay(panel, panel_io);
#endif
        backlight_ = new PwmBacklight(DISPLAY_BACKLIGHT_PIN, DISPLAY_BACKLIGHT_OUTPUT_INVERT);
        backlight_->RestoreBrightness();
    }

    void InitializeButtons()
    {
        boot_button_.OnClick([this]() {
            auto &app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting && !WifiStation::GetInstance().IsConnected()) {
                ESP_LOGI(TAG, "Boot button pressed, enter WiFi configuration mode");
                ResetWifiConfiguration();
            }
            app.ToggleChatState();
        });
        gpio_config_t power_gpio_config = {
            .pin_bit_mask = (BIT64(POWER_CTRL)),
            .mode = GPIO_MODE_OUTPUT,

        };
        ESP_ERROR_CHECK(gpio_config(&power_gpio_config));

        gpio_set_level(POWER_CTRL, 0);
    }

public:
    EspS3Cat() : boot_button_(BOOT_BUTTON_GPIO)
    {
        InitializeI2c();
        uint8_t pcb_verison = DetectPcbVersion();
        InitializeCharge();
        InitializeCst816sTouchPad();

        InitializeSpi();
        Initializest77916Display(pcb_verison);
        InitializeButtons();
    }

    virtual AudioCodec* GetAudioCodec() override
    {
        static BoxAudioCodec audio_codec(
            i2c_bus_,
            AUDIO_INPUT_SAMPLE_RATE,
            AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_GPIO_MCLK,
            AUDIO_I2S_GPIO_BCLK,
            AUDIO_I2S_GPIO_WS,
            AUDIO_I2S_GPIO_DOUT,
            AUDIO_I2S_GPIO_DIN,
            AUDIO_CODEC_PA_PIN,
            AUDIO_CODEC_ES8311_ADDR,
            AUDIO_CODEC_ES7210_ADDR,
            AUDIO_INPUT_REFERENCE);
        return &audio_codec;
    }

    virtual Display* GetDisplay() override
    {
        return display_;
    }

    virtual bool GetBatteryLevel(int& level, bool& charging, bool& discharging) override
    {
        return charge_ != nullptr && charge_->GetBatteryLevel(level, charging, discharging);
    }

    Cst816s* GetTouchpad()
    {
        return cst816s_;
    }

    virtual Backlight* GetBacklight() override
    {
        return backlight_;
    }
};

DECLARE_BOARD(EspS3Cat);
