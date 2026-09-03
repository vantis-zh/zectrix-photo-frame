#include "application.h"

#include "boards/zectrix-s3-epaper-4.2/custom_lcd_display.h"
#include "boards/zectrix-s3-epaper-4.2/config.h"
#include "board.h"
#include "common/frame_settings.h"
#include "common/photo_storage.h"
#include "common/remote_photo_service.h"
#include "display.h"
#include "settings.h"
#include "ui/rawdraw_ui_manager.h"
#include "wifi_manager.h"
#include "wifi_configuration_ap.h"

#include <esp_mac.h>
#include <esp_log.h>
#include <esp_sleep.h>
#include <esp_sntp.h>
#include <esp_system.h>
#include <esp_wifi.h>

#include <ctime>

namespace {

constexpr char kTag[] = "Application";
constexpr char kSyncNamespace[] = "sync";
constexpr char kSyncIntervalKey[] = "sync_interval";
// Item indexes for the trimmed settings list. Keep in sync with the
// items.push_back order in Initialize(): 0 系统 1 重启 2 图片(section)
// 3 拉取新图 4 图片来源 5 时区 6 自动刷新 7 关于 8 固件
constexpr int kSettingsFetchNowIndex = 3;
constexpr int kSettingsImageSourceIndex = 4;
constexpr int kSettingsTzIndex = 5;
constexpr int kSettingsRefreshIndex = 6;

std::string FormatImageSourceLabel(const std::string& url) {
    if (url.empty() || url.find("loremflickr") != std::string::npos) return "默认";
    if (url.find("picsum") != std::string::npos) return "源2";
    return "源1";
}

void StartSntpClockSyncOnce() {
    static bool s_started = false;
    if (s_started) return;

    // SNTP always syncs in UTC; TZ only affects local display and
    // "fixed time" auto-refresh semantics (see FrameSettings).
    const std::string tz = FrameSettings::GetTz();
    setenv("TZ", tz.c_str(), 1);
    tzset();
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "ntp.aliyun.com");
    esp_sntp_setservername(1, "cn.pool.ntp.org");
    esp_sntp_setservername(2, "pool.ntp.org");
    esp_sntp_set_time_sync_notification_cb([](struct timeval*) {
        time_t now = 0;
        time(&now);
        struct tm local_tm = {};
        localtime_r(&now, &local_tm);
        char time_buf[32] = {};
        strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S", &local_tm);
        ESP_LOGI(kTag, "SNTP time synchronized: %s", time_buf);
        Application::GetInstance().UpdateStatusBarForUi();
    });
    esp_sntp_init();
    s_started = true;
    ESP_LOGI(kTag, "SNTP started: tz=%s servers=ntp.aliyun.com,cn.pool.ntp.org,pool.ntp.org", tz.c_str());
}

bool IsLocalHttpServiceRunning(const ui::RawDrawUiManager* manager) {
    return manager != nullptr && manager->IsHttpServerRunning();
}

}  // namespace

Application::Application() = default;

Application::~Application() {
    if (sleep_timer_ != nullptr) {
        esp_timer_stop(sleep_timer_);
        esp_timer_delete(sleep_timer_);
        sleep_timer_ = nullptr;
    }
}

void Application::Initialize() {
    auto& board = Board::GetInstance();
    SetDeviceState(kDeviceStateStarting);

    AudioCodec* codec = board.GetAudioCodec();
    if (codec == nullptr) {
        ESP_LOGE(kTag, "Audio codec is null");
        SetDeviceState(kDeviceStateFatalError);
        return;
    }

    audio_service_.Initialize(codec);
    audio_service_.Start();

    Display* display = board.GetDisplay();
    if (display == nullptr) {
        ESP_LOGW(kTag, "No display available, skipping init");
        SetDeviceState(kDeviceStateFatalError);
        return;
    }
    if (photo_storage_init() == 0) {
        ESP_LOGI(kTag, "Photo storage ready (%d photos)", photo_get_count());
    } else {
        ESP_LOGW(kTag, "Photo storage init failed");
    }

    auto* lcd = static_cast<CustomLcdDisplay*>(display);
    rawdraw_ui_manager_ = std::make_unique<ui::RawDrawUiManager>();
    rawdraw_ui_manager_->Init(lcd, [lcd](const rawdraw::Rect&, bool urgent) {
        if (urgent) {
            lcd->RequestUrgentFullRefresh();
        } else {
            lcd->RequestUrgentRefresh();
        }
    });

    if (auto* sr = rawdraw_ui_manager_->GetSettingsRenderer()) {
        std::vector<rawdraw::SettingsItemDef> items;
        items.push_back({"系统", "", nullptr, rawdraw::SettingsItemType::Section, false});
        items.push_back({"重启", "执行", nullptr, rawdraw::SettingsItemType::Action, false,
                         []() { esp_restart(); }});
        items.push_back({"图片", "", nullptr, rawdraw::SettingsItemType::Section, false});
        items.push_back({"拉取新图", "执行", nullptr,
                         rawdraw::SettingsItemType::Action, false,
                         []() {
                             ui::RawDrawUiManager::RequestRemotePhotoRefresh();
                         }});
        items.push_back({"图片来源", FormatImageSourceLabel(RemotePhotoService::GetImageUrl()), nullptr,
                         rawdraw::SettingsItemType::Action, false,
                         [sr]() {
                             // Cycle: default -> image1 -> image2 -> default.
                             // Replace the placeholders below with real URLs
                             // (e.g. the FnOS bridge) once available.
                             static const char* kSources[] = {
                                 "",  // default (loremflickr)
                                 "http://192.168.1.10:8787/random.jpg",
                                 "https://picsum.photos/{W}/{H}",
                             };
                             static const char* kLabels[] = {"默认", "源1", "源2"};
                             // Start from the currently active source
                             static int s_index = 0;
                             const std::string current = RemotePhotoService::GetImageUrl();
                             for (int i = 0; i < 3; ++i) {
                                 if (current == kSources[i]) { s_index = i; break; }
                             }
                             s_index = (s_index + 1) % 3;
                             RemotePhotoService::SetImageUrl(kSources[s_index]);
                             if (sr) {
                                 sr->UpdateItem(kSettingsImageSourceIndex, kLabels[s_index]);
                             }
                         }});
        // Timezone: pick from the built-in list, saved to NVS ("frame"/tz).
        // Takes effect immediately (setenv TZ + tzset) — SNTP always syncs
        // in UTC, so only local display / "fixed time" semantics change.
        {
            std::string tz_label;
            FrameSettings::FormatTzLabel(FrameSettings::GetTz(), tz_label);
            items.push_back({"时区", tz_label, nullptr,
                             rawdraw::SettingsItemType::Action, false,
                             [sr]() {
                                 if (!sr) return;
                                 std::vector<std::string> options;
                                 for (int i = 0; i < FrameSettings::kTimeZoneCount; ++i) {
                                     options.push_back(FrameSettings::kTimeZoneList[i].label);
                                 }
                                 const int cur_idx = FrameSettings::GetTzIndex();
                                 sr->ShowListDialog("设置时区", options,
                                                    options[cur_idx]);
                                 sr->SetListDialogHandler(
                                     [sr](int index) {
                                         if (index < 0 ||
                                             index >= FrameSettings::kTimeZoneCount) {
                                             return;  // cancelled
                                         }
                                         const char* new_tz =
                                             FrameSettings::kTimeZoneList[index].posix;
                                         FrameSettings::SetTz(new_tz);
                                         // Apply immediately in this session:
                                         // SNTP stays UTC, but the status-bar
                                         // clock and "fixed time" auto-refresh
                                         // now use the new local timezone.
                                         setenv("TZ", new_tz, 1);
                                         tzset();
                                         if (sr) {
                                             sr->UpdateItem(kSettingsTzIndex,
                                                            FrameSettings::kTimeZoneList[index].label);
                                         }
                                     });
                             }});
        }
        // Auto refresh mode: off / interval / fixed daily time, saved to
        // NVS ("frame"). Used by EnterScheduledSleep to arm the RTC wakeup.
        {
            std::string refresh_label;
            FrameSettings::FormatAutoRefresh(FrameSettings::GetAutoRefresh(),
                                             refresh_label);
            items.push_back({"自动刷新", refresh_label, nullptr,
                             rawdraw::SettingsItemType::Action, false,
                             [sr]() {
                                 if (!sr) return;
                                 static const std::vector<std::string> kModes = {
                                     "关闭", "间隔时长", "固定时间"};
                                 const FrameSettings::AutoRefreshConfig cur =
                                     FrameSettings::GetAutoRefresh();
                                 const std::string cur_label =
                                     cur.mode == FrameSettings::kRefreshModeOff
                                         ? "关闭"
                                         : (cur.mode == FrameSettings::kRefreshModeInterval
                                                ? "间隔时长"
                                                : "固定时间");
                                 sr->ShowListDialog("自动刷新", kModes, cur_label);
                                 sr->SetListDialogHandler([sr](int index) {
                                     if (index < 0) return;  // cancelled
                                     if (index == 0) {
                                         // Off: still sleep for power saving,
                                         // but no timer wakeup (BOOT only).
                                         FrameSettings::AutoRefreshConfig cfg =
                                             FrameSettings::GetAutoRefresh();
                                         cfg.mode = FrameSettings::kRefreshModeOff;
                                         FrameSettings::SetAutoRefresh(cfg);
                                         std::string label;
                                         FrameSettings::FormatAutoRefresh(cfg, label);
                                         sr->UpdateItem(kSettingsRefreshIndex, label);
                                         return;
                                     }
                                     if (index == 1) {
                                         // Interval mode: mixed duration
                                         // presets (value + unit in one pick).
                                         struct IntervalPreset {
                                             int interval;
                                             int unit;
                                             const char* label;
                                         };
                                         static const IntervalPreset kPresets[] = {
                                             {15, FrameSettings::kUnitMinutes, "15 分钟"},
                                             {30, FrameSettings::kUnitMinutes, "30 分钟"},
                                             {1, FrameSettings::kUnitHours, "1 小时"},
                                             {2, FrameSettings::kUnitHours, "2 小时"},
                                             {6, FrameSettings::kUnitHours, "6 小时"},
                                             {12, FrameSettings::kUnitHours, "12 小时"},
                                             {24, FrameSettings::kUnitHours, "24 小时"},
                                         };
                                         constexpr int kPresetCount =
                                             static_cast<int>(sizeof(kPresets) / sizeof(kPresets[0]));
                                         std::vector<std::string> preset_labels;
                                         const FrameSettings::AutoRefreshConfig cur2 =
                                             FrameSettings::GetAutoRefresh();
                                         std::string cur_preset_label = kPresets[0].label;
                                         for (int i = 0; i < kPresetCount; ++i) {
                                             preset_labels.push_back(kPresets[i].label);
                                             if (cur2.mode == FrameSettings::kRefreshModeInterval &&
                                                 cur2.interval == kPresets[i].interval &&
                                                 cur2.unit == kPresets[i].unit) {
                                                 cur_preset_label = kPresets[i].label;
                                             }
                                         }
                                         sr->ShowListDialog("间隔时长", preset_labels,
                                                            cur_preset_label);
                                         sr->SetListDialogHandler([sr](int preset_idx) {
                                             if (preset_idx < 0 || preset_idx >= kPresetCount) {
                                                 return;  // cancelled
                                             }
                                             FrameSettings::AutoRefreshConfig cfg =
                                                 FrameSettings::GetAutoRefresh();
                                             cfg.mode = FrameSettings::kRefreshModeInterval;
                                             cfg.interval = kPresets[preset_idx].interval;
                                             cfg.unit = kPresets[preset_idx].unit;
                                             FrameSettings::SetAutoRefresh(cfg);
                                             std::string label;
                                             FrameSettings::FormatAutoRefresh(cfg, label);
                                             sr->UpdateItem(kSettingsRefreshIndex, label);
                                         });
                                         return;
                                     }
                                     if (index == 2) {
                                         // Fixed-time mode: three-field
                                         // editor (hour -> minute -> confirm).
                                         FrameSettings::AutoRefreshConfig cfg =
                                             FrameSettings::GetAutoRefresh();
                                         sr->ShowTimeSetDialog(cfg.minutes_of_day);
                                         sr->SetTimeSetDialogHandler([sr](int minutes_of_day) {
                                             FrameSettings::AutoRefreshConfig cfg2 =
                                                 FrameSettings::GetAutoRefresh();
                                             cfg2.mode = FrameSettings::kRefreshModeFixedTime;
                                             cfg2.minutes_of_day = minutes_of_day;
                                             FrameSettings::SetAutoRefresh(cfg2);
                                             std::string label;
                                             FrameSettings::FormatAutoRefresh(cfg2, label);
                                             sr->UpdateItem(kSettingsRefreshIndex, label);
                                         });
                                         return;
                                     }
                                 });
                             }});
        }
        items.push_back({"关于", "", nullptr, rawdraw::SettingsItemType::Section, false});
        items.push_back({"固件", PROJECT_VER, nullptr, rawdraw::SettingsItemType::Normal, false});
        sr->SetItems(items);
        sr->SetFirmwareVersion("v" PROJECT_VER);

        uint8_t mac_bytes[6] = {};
        esp_read_mac(mac_bytes, ESP_MAC_WIFI_STA);
        char mac_str[18];
        snprintf(mac_str, sizeof(mac_str), "%02X:%02X:%02X:%02X:%02X:%02X",
                 mac_bytes[0], mac_bytes[1], mac_bytes[2],
                 mac_bytes[3], mac_bytes[4], mac_bytes[5]);
        sr->SetDeviceInfo(mac_str, "ESP32-S3");
    }

    ESP_LOGI(kTag, "Rawdraw gallery UI initialized");
    wake_cause_ = esp_sleep_get_wakeup_cause();
    if (esp_reset_reason() == ESP_RST_DEEPSLEEP) {
        ESP_LOGI(kTag, "Wake from deep sleep: cause=%d (2=ext0/BOOT, 4=timer), refresh UI",
                 static_cast<int>(wake_cause_));
        board.FlashActivityLed();
        if (rawdraw_ui_manager_) {
            rawdraw_ui_manager_->RequestActivePageRefresh();
        }
    }

    // Set up WiFi status callback to update StatusBar
    board.SetNetworkEventCallback([this](NetworkEvent event, const std::string& data) {
        switch (event) {
            case NetworkEvent::Connected:
                ESP_LOGI(kTag, "WiFi connected: %s", data.c_str());
                wifi_connected_.store(true, std::memory_order_release);
                StartSntpClockSyncOnce();
                // First remote photo fetch after boot. BOOT click on the
                // gallery also triggers it. Scheduled auto refresh (interval
                // or fixed time, user-configurable) is handled by the RTC
                // timer wake-up (see EnterScheduledSleep).
                if (esp_reset_reason() == ESP_RST_DEEPSLEEP &&
                    wake_cause_ == ESP_SLEEP_WAKEUP_TIMER) {
                    // Scheduled auto refresh: fetch automatically.
                    ESP_LOGI(kTag, "auto refresh timer wake-up: fetching new photo");
                    RemotePhotoService::GetInstance().RequestRefresh("auto-refresh");
                } else {
                    // Manual wake (BOOT) or power-on: show stored photo,
                    // no automatic fetch.
                    ESP_LOGI(kTag, "wake-up not timer-scheduled: keep stored photo");
                }
                if (rawdraw_ui_manager_ &&
                    rawdraw_ui_manager_->GetCurrentPage() == ui::RawDrawPageId::APTransfer) {
                    ESP_LOGI(kTag, "WiFi connected while config page is visible, returning to gallery");
                    rawdraw_ui_manager_->SwitchPage(ui::RawDrawPageId::Gallery);
                }
                UpdateStatusBarForUi();
                // After a scheduled auto refresh, go back to sleep
                // quickly (photo already fetched/EPD refreshing); other
                // boots keep the user-configured sync interval.
                ArmSyncSleepTimer(esp_reset_reason() == ESP_RST_DEEPSLEEP &&
                                          wake_cause_ == ESP_SLEEP_WAKEUP_TIMER
                                      ? kPostRefreshAwakeMinutes
                                      : -1);
                break;
            case NetworkEvent::Disconnected:
                ESP_LOGI(kTag, "WiFi disconnected");
                wifi_connected_.store(false, std::memory_order_release);
                UpdateStatusBarForUi();
                break;
            case NetworkEvent::Connecting:
            case NetworkEvent::Scanning:
                wifi_connected_.store(false, std::memory_order_release);
                UpdateStatusBarForUi();
                break;
            case NetworkEvent::WifiConfigModeEnter:
                ESP_LOGI(kTag, "WiFi config mode entered: %s", data.c_str());
                wifi_connected_.store(false, std::memory_order_release);
                if (rawdraw_ui_manager_) {
                    auto& wifi = WifiManager::GetInstance();
                    rawdraw_ui_manager_->ShowWifiConfigPage(wifi.GetApSsid(),
                                                            wifi.GetApPassword(),
                                                            wifi.GetApWebUrl());
                }
                UpdateStatusBarForUi();
                break;
            case NetworkEvent::WifiConfigModeExit:
                if (rawdraw_ui_manager_ &&
                    rawdraw_ui_manager_->GetCurrentPage() == ui::RawDrawPageId::APTransfer) {
                    ESP_LOGI(kTag, "WiFi config AP exited, returning to gallery");
                    rawdraw_ui_manager_->SwitchPage(ui::RawDrawPageId::Gallery);
                }
                wifi_connected_.store(WifiManager::GetInstance().IsConnected(),
                                      std::memory_order_release);
                UpdateStatusBarForUi();
                break;
            case NetworkEvent::OpenDeviceSettings:
                ESP_LOGI(kTag, "Open device settings requested from web portal");
                if (rawdraw_ui_manager_) {
                    // Jump to the on-device settings page; the user
                    // continues on the screen (UP long press returns to
                    // the gallery as usual).
                    rawdraw_ui_manager_->SwitchPage(ui::RawDrawPageId::Settings);
                }
                break;
            case NetworkEvent::ModemDetecting:
            case NetworkEvent::ModemErrorNoSim:
            case NetworkEvent::ModemErrorRegDenied:
            case NetworkEvent::ModemErrorInitFailed:
            case NetworkEvent::ModemErrorTimeout:
                wifi_connected_.store(false, std::memory_order_release);
                UpdateStatusBarForUi();
                break;
        }
    });

    // Remote photo service: worker task + 24h auto refresh timer.
    // On fetch completion, pull the new photo into the gallery selection and
    // refresh the visible page (callback fires on the worker task; the UI
    // manager queues the actual EPD work to the main loop internally).
    auto& remote_photo = RemotePhotoService::GetInstance();
    remote_photo.SetStateCallback([this](RemotePhotoService::State state,
                                         const std::string& message) {
        if (state != RemotePhotoService::kDone) {
            if (state == RemotePhotoService::kError) {
                ESP_LOGW(kTag, "remote photo fetch failed: %s", message.c_str());
            }
            return;
        }
        if (!rawdraw_ui_manager_) {
            return;
        }
        rawdraw_ui_manager_->OnRemotePhotoStored();
    });
    remote_photo.Start();

    // Web portal "Other" tab: expose frame settings (timezone + auto refresh)
    // over the config-AP HTTP server. Query runs in the httpd handler context;
    // save runs in a deferred task (both are fine for NVS/setenv access).
    WifiManager::GetInstance().SetOnFrameSettingsQuery([]() {
        FrameSettingsState state;
        state.tz = FrameSettings::GetTz();
        const FrameSettings::AutoRefreshConfig cfg = FrameSettings::GetAutoRefresh();
        state.refresh_mode = cfg.mode;
        // Normalize the NVS interval (value + unit) to minutes for the web UI.
        state.refresh_interval_minutes =
            cfg.unit == FrameSettings::kUnitHours ? cfg.interval * 60 : cfg.interval;
        state.refresh_time = cfg.minutes_of_day;
        state.image_source = RemotePhotoService::GetImageUrl();
        return state;
    });
    WifiManager::GetInstance().SetOnFrameSettingsSave([](const FrameSettingsState& s) {
        FrameSettings::SetTz(s.tz);
        // SNTP always syncs in UTC; TZ only affects local display and the
        // "fixed time" refresh semantics — apply it immediately.
        setenv("TZ", s.tz.c_str(), 1);
        tzset();
        FrameSettings::AutoRefreshConfig cfg;
        cfg.mode = s.refresh_mode;
        // Best-fit unit for the NVS representation: store whole hours when
        // the minutes value divides evenly (>= 60), minutes otherwise.
        if (s.refresh_interval_minutes >= 60 &&
            s.refresh_interval_minutes % 60 == 0) {
            cfg.interval = s.refresh_interval_minutes / 60;
            cfg.unit = FrameSettings::kUnitHours;
        } else {
            cfg.interval = s.refresh_interval_minutes;
            cfg.unit = FrameSettings::kUnitMinutes;
        }
        cfg.minutes_of_day = s.refresh_time;
        FrameSettings::SetAutoRefresh(cfg);
        // Image source: "" erases the NVS key and falls back to the built-in
        // default (loremflickr); otherwise the URL template is stored as-is.
        RemotePhotoService::SetImageUrl(s.image_source);
        ESP_LOGI(kTag, "Frame settings saved from web: tz=%s mode=%d interval=%dmin time=%d src=%s",
                 s.tz.c_str(), s.refresh_mode, s.refresh_interval_minutes,
                 s.refresh_time, s.image_source.c_str());
    });

    // Start network (non-blocking, WiFi connects asynchronously)
    board.RequestNetwork();

    SetDeviceState(kDeviceStateIdle);
}

void Application::OnUpClick() {
    ESP_LOGI(kTag, "UP click");
    Board::GetInstance().FlashActivityLed();
    if (rawdraw_ui_manager_) {
        rawdraw_ui_manager_->HandleInput(rawdraw::ButtonEvent{rawdraw::ButtonEvent::kUpClick});
    }
}

void Application::OnDownClick() {
    ESP_LOGI(kTag, "DOWN click");
    Board::GetInstance().FlashActivityLed();
    if (rawdraw_ui_manager_) {
        rawdraw_ui_manager_->HandleInput(rawdraw::ButtonEvent{rawdraw::ButtonEvent::kDownClick});
    }
}

void Application::OnUpLongPress() {
    ESP_LOGI(kTag, "UP long press");
    NoteButtonActivity();
    if (rawdraw_ui_manager_ &&
        rawdraw_ui_manager_->GetCurrentPage() == ui::RawDrawPageId::Settings) {
        ESP_LOGI(kTag, "UP long press - leaving settings");
        rawdraw_ui_manager_->SwitchPage(ui::RawDrawPageId::Gallery);
    }
}

void Application::OnDownLongPress() {
    ESP_LOGI(kTag, "DOWN long press");
    NoteButtonActivity();
    if (rawdraw_ui_manager_) {
        ESP_LOGI(kTag, "DOWN long press - entering settings");
        rawdraw_ui_manager_->SwitchPage(ui::RawDrawPageId::Settings);
    }
}

void Application::OnWifiConfigComboLongPress() {
    ESP_LOGI(kTag, "UP+DOWN long press");
    NoteButtonActivity();
    EnterWifiConfigMode();
}

void Application::OnBootClick() {
    ESP_LOGI(kTag, "BOOT click");
    Board::GetInstance().FlashActivityLed();
    if (rawdraw_ui_manager_) {
        rawdraw_ui_manager_->HandleInput(rawdraw::ButtonEvent{rawdraw::ButtonEvent::kBootClick});
    }
}

void Application::OnBootLongPress() {
    ESP_LOGI(kTag, "BOOT long press");
    NoteButtonActivity();
    if (WifiManager::GetInstance().IsConfigMode()) {
        ESP_LOGI(kTag, "BOOT long press - exiting WiFi config AP");
        if (rawdraw_ui_manager_) {
            rawdraw_ui_manager_->SwitchPage(ui::RawDrawPageId::Gallery);
        }
        WifiManager::GetInstance().StartStation();
        return;
    }
    if (rawdraw_ui_manager_) {
        rawdraw_ui_manager_->HandleInput(rawdraw::ButtonEvent{rawdraw::ButtonEvent::kBootLongPress});
    }
}

void Application::NoteButtonActivity() {
    Board::GetInstance().FlashActivityLed();
    if (rawdraw_ui_manager_) {
        rawdraw_ui_manager_->RequestActivePageRefresh();
    }
}

void Application::EnterWifiConfigMode() {
    if (rawdraw_ui_manager_ && rawdraw_ui_manager_->IsLanHttpServerRunning()) {
        rawdraw_ui_manager_->StopLanHttpServer();
    }
    wifi_connected_.store(false, std::memory_order_release);
    ESP_LOGI(kTag, "Entering WiFi config mode by long press");
    WifiManager::GetInstance().StartConfigAp();
    if (rawdraw_ui_manager_ && WifiManager::GetInstance().IsConfigMode()) {
        auto& wifi = WifiManager::GetInstance();
        rawdraw_ui_manager_->ShowWifiConfigPage(wifi.GetApSsid(),
                                                wifi.GetApPassword(),
                                                wifi.GetApWebUrl());
    }
    UpdateStatusBarForUi();
}

void Application::ArmSyncSleepTimer(int override_minutes) {
    int interval_minutes = override_minutes;
    if (interval_minutes <= 0) {
        Settings nvs(kSyncNamespace, false);
        interval_minutes = nvs.GetInt(kSyncIntervalKey, 30);
    }
    if (interval_minutes <= 0) {
        ESP_LOGI(kTag, "Sync sleep interval: 关闭");
        return;
    }
    if (sleep_timer_ == nullptr) {
        esp_timer_create_args_t args = {};
        args.callback = [](void* arg) {
            static_cast<Application*>(arg)->EnterScheduledSleep();
        };
        args.arg = this;
        args.dispatch_method = ESP_TIMER_TASK;
        args.name = "app_sync_sleep";
        ESP_ERROR_CHECK(esp_timer_create(&args, &sleep_timer_));
    }
    esp_timer_stop(sleep_timer_);
    const int64_t delay_us = static_cast<int64_t>(interval_minutes) * 60 * 1000 * 1000;
    ESP_LOGI(kTag, "Sync sleep interval: %d minutes", interval_minutes);
    ESP_LOGI(kTag, "Scheduling sleep after sync interval: %d minutes", interval_minutes);
    ESP_ERROR_CHECK(esp_timer_start_once(sleep_timer_, delay_us));
}

void Application::EnterScheduledSleep() {
    const bool scheduled_refresh =
        wake_cause_ == ESP_SLEEP_WAKEUP_TIMER;
    // After a scheduled auto refresh, do not honor the 30-min sync
    // interval: arm a short "cooling" timer so the device goes back to
    // sleep and the next wake lands on the following schedule point.
    const int override_minutes = scheduled_refresh ? kPostRefreshAwakeMinutes : -1;
    ESP_LOGI(kTag, "Scheduling sleep: override_minutes=%d (scheduled_refresh=%d)",
             override_minutes, scheduled_refresh ? 1 : 0);

    // RTC timer wakes the device for the next auto refresh (interval or
    // fixed local time); on wake the normal boot path runs
    // (WiFi -> fetch new photo -> EPD refresh). With auto refresh off, no
    // timer wakeup is armed: only the BOOT button can wake the device.
    const FrameSettings::AutoRefreshConfig cfg = FrameSettings::GetAutoRefresh();
    const int64_t next_refresh_us = FrameSettings::MicrosUntilNextRefresh(cfg, time(nullptr));
    if (next_refresh_us >= 0) {
        esp_sleep_enable_timer_wakeup(static_cast<uint64_t>(next_refresh_us));
        ESP_LOGI(kTag, "RTC wake-up armed for auto refresh (%lld min)",
                 (long long)(next_refresh_us / 60000000LL));
    } else {
        ESP_LOGI(kTag, "auto refresh disabled: timer wake-up not armed (BOOT only)");
    }

    ESP_LOGI(kTag, "Entering deep sleep; BOOT wakes device, timer wakes for auto refresh");
    wifi_connected_.store(false, std::memory_order_release);
    esp_wifi_disconnect();
    esp_wifi_stop();
    esp_sleep_enable_ext0_wakeup(static_cast<gpio_num_t>(BOOT_BUTTON_GPIO), 0);
    esp_deep_sleep_start();
}

void Application::EnterManualSleep() {
    ESP_LOGI(kTag, "Entering manual deep sleep; stopping local services and WiFi");
    if (sleep_timer_ != nullptr) {
        esp_timer_stop(sleep_timer_);
    }
    if (rawdraw_ui_manager_) {
        rawdraw_ui_manager_->StopLanHttpServer();
    }
    wifi_connected_.store(false, std::memory_order_release);
    esp_wifi_disconnect();
    esp_wifi_stop();
    UpdateStatusBarForUi();
    vTaskDelay(pdMS_TO_TICKS(300));
    esp_sleep_enable_ext0_wakeup(static_cast<gpio_num_t>(BOOT_BUTTON_GPIO), 0);
    esp_deep_sleep_start();
}

void Application::Run() {
    while (true) {
        if (rawdraw_ui_manager_) {
            rawdraw_ui_manager_->PumpClockRefresh();
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

bool Application::SetDeviceState(DeviceState state) {
    const DeviceState old_state = state_.exchange(state, std::memory_order_acq_rel);
    ESP_LOGI(kTag, "State %d -> %d", old_state, state);
    return true;
}

void Application::Schedule(std::function<void()>&& callback) {
    if (callback) {
        callback();
    }
}

void Application::PlaySound(const std::string_view& sound) {
    audio_service_.PlaySound(sound);
}

void Application::PlaySound(const std::string_view& sound, int duration_ms) {
    audio_service_.PlaySound(sound, duration_ms);
}

void Application::MuteSound() {
    audio_service_.MuteOutput();
}

void Application::StopSound() {
    audio_service_.ResetDecoder();
}

bool Application::CanEnterSleepMode() const {
    return false;
}

void Application::UpdateStatusBarForUi() {
    auto& board = Board::GetInstance();
    int battery_level = -1;
    bool charging = false;
    bool discharging = false;
    board.GetBatteryLevel(battery_level, charging, discharging);

    if (rawdraw_ui_manager_) {
        const bool wifi_connected = wifi_connected_.load(std::memory_order_acquire);
        ui::RawDrawStatusBarData data = rawdraw_ui_manager_->GetStatusBarData();
        data.page_title = ui::RawDrawUiManager::GetPageTitle(rawdraw_ui_manager_->GetCurrentPage());
        data.wifi_connected = wifi_connected;
        data.server_connected = false;
        data.battery_level = battery_level;
        data.battery_charging = charging;
        rawdraw_ui_manager_->UpdateStatusBar(data);
        rawdraw_ui_manager_->RequestActivePageRefresh();
    }
    return;
}
