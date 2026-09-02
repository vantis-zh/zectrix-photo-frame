#ifndef _APPLICATION_H_
#define _APPLICATION_H_

#include <atomic>
#include <functional>
#include <memory>
#include <string_view>

#include <esp_sleep.h>

#include "audio_service.h"
#include "device_state.h"

namespace ui {
class RawDrawUiManager;
}

class Application {
public:
    static Application& GetInstance() {
        static Application instance;
        return instance;
    }

    Application(const Application&) = delete;
    Application& operator=(const Application&) = delete;

    void Initialize();
    void Run();

    DeviceState GetDeviceState() const { return state_.load(std::memory_order_acquire); }
    bool SetDeviceState(DeviceState state);

    void Schedule(std::function<void()>&& callback);
    void PlaySound(const std::string_view& sound);
    void PlaySound(const std::string_view& sound, int duration_ms);
    void MuteSound();
    void StopSound();
    bool CanEnterSleepMode() const;

    AudioService& GetAudioService() { return audio_service_; }
    ui::RawDrawUiManager* GetRawDrawUiManager() { return rawdraw_ui_manager_.get(); }
    void UpdateStatusBarForUi();
    void OnUpClick();
    void OnDownClick();
    void OnUpLongPress();
    void OnDownLongPress();
    void OnWifiConfigComboLongPress();
    void OnBootClick();
    void OnBootLongPress();

private:
    Application();
    ~Application();

    std::atomic<DeviceState> state_{kDeviceStateUnknown};
    std::atomic<bool> wifi_connected_{false};
    AudioService audio_service_;
    std::unique_ptr<ui::RawDrawUiManager> rawdraw_ui_manager_;
    esp_timer_handle_t sleep_timer_ = nullptr;
    esp_sleep_wakeup_cause_t wake_cause_ = ESP_SLEEP_WAKEUP_UNDEFINED;

    // Minutes to stay awake after a scheduled (auto-refresh) wake before
    // going back to deep sleep. Long enough for download + EPD refresh.
    static constexpr int kPostRefreshAwakeMinutes = 3;

    // override_minutes > 0 replaces the NVS sync interval (used after a
    // scheduled auto refresh so the device sleeps again quickly).
    void ArmSyncSleepTimer(int override_minutes = -1);
    void EnterScheduledSleep();
    void EnterManualSleep();
    void NoteButtonActivity();
    void EnterWifiConfigMode();
};

#endif  // _APPLICATION_H_
