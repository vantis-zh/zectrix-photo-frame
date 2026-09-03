#include <stdio.h>
#include <driver/gpio.h>
#include <driver/usb_serial_jtag.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include "board_power_bsp.h"
#include "charge_status.h"

// 绿色 LED 挂在 GPIO3，低电平点亮（驱动低电平才有电流流过 LED）。
static constexpr int kLedOnLevel  = 0;
static constexpr int kLedOffLevel = 1;

void BoardPowerBsp::PowerLedTask(void *arg) {
    auto* self = static_cast<BoardPowerBsp*>(arg);
    gpio_config_t gpio_conf = {};
    gpio_conf.intr_type     = GPIO_INTR_DISABLE;
    gpio_conf.mode          = GPIO_MODE_OUTPUT;
    gpio_conf.pin_bit_mask  = (0x1ULL << GPIO_NUM_3);
    gpio_conf.pull_down_en  = GPIO_PULLDOWN_DISABLE;
    gpio_conf.pull_up_en    = GPIO_PULLUP_DISABLE;
    ESP_ERROR_CHECK_WITHOUT_ABORT(gpio_config(&gpio_conf));
    // 输出默认为 0（=亮），先置暗，避免开机窗口闪一下。
    gpio_set_level(GPIO_NUM_3, kLedOffLevel);
    for (;;) {
        if (self->led_override_enabled_.load(std::memory_order_relaxed)) {
            const bool blink = self->led_override_blink_.load(std::memory_order_relaxed);
            if (blink) {
                const bool phase = !self->led_override_phase_.load(std::memory_order_relaxed);
                self->led_override_phase_.store(phase, std::memory_order_relaxed);
                gpio_hold_dis((gpio_num_t)GPIO_NUM_3);
                gpio_set_level(GPIO_NUM_3, phase ? kLedOnLevel : kLedOffLevel);
                gpio_hold_en((gpio_num_t)GPIO_NUM_3);
                vTaskDelay(pdMS_TO_TICKS(500));
                continue;
            }
            gpio_hold_dis((gpio_num_t)GPIO_NUM_3);
            gpio_set_level(GPIO_NUM_3, kLedOnLevel);
            gpio_hold_en((gpio_num_t)GPIO_NUM_3);
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }

        ChargeStatus::Snapshot snap{};
        const bool has_status = self && self->charge_status_;
        if (has_status) {
            self->charge_status_->Tick(esp_timer_get_time() / 1000);
            snap = self->charge_status_->Get();
        }

        // Dev/flash session: LED solid on while a USB host (Serial-JTAG) is
        // attached. Covers plugging in to flash and the post-flash reboot;
        // only the ROM download window itself stays dark (user code is not
        // running there, so the pad state is beyond firmware control).
        const bool usb_connected = usb_serial_jtag_is_connected();
        // DEBUG 级别，默认日志等级下不可见；调高 BoardPowerBsp 日志等级可排查 LED 分支。
        static int s_log_countdown = 0;
        if (--s_log_countdown <= 0) {
            s_log_countdown = 10;  // 每 ~5s 一条
            ESP_LOGD("BoardPowerBsp", "led: usb=%d charging=%d full=%d no_bat=%d",
                     usb_connected ? 1 : 0, snap.charging ? 1 : 0, snap.full ? 1 : 0,
                     snap.no_battery ? 1 : 0);
        }
        if (usb_connected) {
            gpio_hold_dis((gpio_num_t)GPIO_NUM_3);
            gpio_set_level(GPIO_NUM_3, kLedOnLevel);
            gpio_hold_en((gpio_num_t)GPIO_NUM_3);
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }

        gpio_hold_dis((gpio_num_t)GPIO_NUM_3);
        if ((!has_status || (!snap.charging && !snap.full)) &&
            self->led_activity_pulses_.load(std::memory_order_relaxed) > 0) {
            // Activity feedback: brief lit blip on wake/button press.
            self->led_activity_pulses_.fetch_sub(1, std::memory_order_relaxed);
            gpio_set_level(GPIO_NUM_3, kLedOnLevel);
            vTaskDelay(pdMS_TO_TICKS(120));
            gpio_set_level(GPIO_NUM_3, kLedOffLevel);
            gpio_hold_en((gpio_num_t)GPIO_NUM_3);
            vTaskDelay(pdMS_TO_TICKS(180));
        } else if (has_status && snap.charging) {
            // Charging: short lit blip every 3s as the charge indicator.
            gpio_set_level(GPIO_NUM_3, kLedOnLevel);
            vTaskDelay(pdMS_TO_TICKS(200));
            gpio_set_level(GPIO_NUM_3, kLedOffLevel);
            gpio_hold_en((gpio_num_t)GPIO_NUM_3);
            vTaskDelay(pdMS_TO_TICKS(2800));
        } else {
            // Idle / battery full / no charge IC: LED off (device at rest).
            gpio_set_level(GPIO_NUM_3, kLedOffLevel);
            gpio_hold_en((gpio_num_t)GPIO_NUM_3);
            vTaskDelay(pdMS_TO_TICKS(500));
        }
    }
}

BoardPowerBsp::BoardPowerBsp(int epdPowerPin, int audioPowerPin, int audioAmpPin, int vbatPowerPin,
                             ChargeStatus* charge_status)
    : epdPowerPin_(epdPowerPin),
      audioPowerPin_(audioPowerPin),
      audioAmpPin_(audioAmpPin),
      vbatPowerPin_(vbatPowerPin),
      charge_status_(charge_status) {
    gpio_config_t gpio_conf = {};
    gpio_conf.intr_type     = GPIO_INTR_DISABLE;
    gpio_conf.mode          = GPIO_MODE_OUTPUT;
    gpio_conf.pin_bit_mask  = (0x1ULL << epdPowerPin_) | (0x1ULL << audioPowerPin_) | (0x1ULL << audioAmpPin_) | (0x1ULL << vbatPowerPin_);
    gpio_conf.pull_down_en  = GPIO_PULLDOWN_DISABLE;
    gpio_conf.pull_up_en    = GPIO_PULLUP_ENABLE;
    ESP_ERROR_CHECK_WITHOUT_ABORT(gpio_config(&gpio_conf));
    xTaskCreatePinnedToCore(PowerLedTask, "PowerLedTask", 3 * 1024, this, 2, NULL, 0);
}

BoardPowerBsp::~BoardPowerBsp() {
}

void BoardPowerBsp::PowerEpdOn() {
    gpio_hold_dis((gpio_num_t) epdPowerPin_);
    gpio_set_level((gpio_num_t) epdPowerPin_, 1);
    gpio_hold_en((gpio_num_t)epdPowerPin_);
}

void BoardPowerBsp::PowerEpdOff() {
    gpio_hold_dis((gpio_num_t) epdPowerPin_);
    gpio_set_level((gpio_num_t) epdPowerPin_, 0);
    gpio_hold_en((gpio_num_t)epdPowerPin_);
}

void BoardPowerBsp::PowerAmpOn() {
    gpio_hold_dis((gpio_num_t)audioAmpPin_);
    gpio_set_level((gpio_num_t) audioAmpPin_, 1);
    gpio_hold_en((gpio_num_t)audioAmpPin_);
}

void BoardPowerBsp::PowerAmpOff() {
    gpio_hold_dis((gpio_num_t)audioAmpPin_);
    gpio_set_level((gpio_num_t) audioAmpPin_, 0);
    gpio_hold_en((gpio_num_t)audioAmpPin_);
}

void BoardPowerBsp::PowerAudioOn() {
    gpio_hold_dis((gpio_num_t)audioPowerPin_);
    gpio_set_level((gpio_num_t) audioPowerPin_, 1);
    gpio_hold_en((gpio_num_t)audioPowerPin_);
}

void BoardPowerBsp::PowerAudioOff() {
    gpio_hold_dis((gpio_num_t)audioPowerPin_);
    gpio_set_level((gpio_num_t) audioPowerPin_, 0);
    gpio_hold_en((gpio_num_t)audioPowerPin_);
}

void BoardPowerBsp::VbatPowerOn() {
    gpio_hold_dis((gpio_num_t)vbatPowerPin_);
    gpio_set_level((gpio_num_t) vbatPowerPin_, 1);
    gpio_hold_en((gpio_num_t)vbatPowerPin_);
}

void BoardPowerBsp::VbatPowerOff() {
    gpio_hold_dis((gpio_num_t)vbatPowerPin_);
    gpio_set_level((gpio_num_t) vbatPowerPin_, 0);
    gpio_hold_en((gpio_num_t)vbatPowerPin_);
}

void BoardPowerBsp::SetFactoryLedOverride(bool enabled, bool blink) {
    led_override_enabled_.store(enabled, std::memory_order_relaxed);
    led_override_blink_.store(blink, std::memory_order_relaxed);
    led_override_phase_.store(false, std::memory_order_relaxed);
}

void BoardPowerBsp::FlashActivityLed() {
    led_activity_pulses_.store(1, std::memory_order_relaxed);
}
