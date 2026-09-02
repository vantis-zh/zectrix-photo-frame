#include "frame_settings.h"

#include <esp_log.h>

#include <algorithm>

#include "settings.h"

namespace {

constexpr const char* kTag = "FrameSettings";
constexpr const char* kNamespace = "frame";
constexpr const char* kKeyTz = "tz";
constexpr const char* kKeyRefreshMode = "refresh_mode";
constexpr const char* kKeyRefreshInterval = "refresh_interval";
constexpr const char* kKeyRefreshUnit = "refresh_unit";
constexpr const char* kKeyRefreshTime = "refresh_time";

constexpr const char* kDefaultTz = "CST-8";

// Interval cap: 7 days in microseconds, guards against overflow/abuse.
constexpr int64_t kMaxRefreshIntervalUs = 7LL * 24 * 60 * 60 * 1000 * 1000;
constexpr int64_t kMinRefreshDelayS = 60;

}  // namespace

// Common timezones. Labels use simple, common Chinese characters to stay
// within the SourceHanSansSC font subset flashed on the device.
const FrameSettings::TimeZoneEntry FrameSettings::kTimeZoneList[] = {
    {"UTC+8 中国", "CST-8"},
    {"UTC+8 新加坡", "CST-8"},
    {"UTC+9 日本", "JST-9"},
    {"UTC+9 韩国", "KST-9"},
    {"UTC+7 泰国越南", "ICT-7"},
    {"UTC+5.5 印度", "IST-5:30"},
    {"UTC+4 迪拜", "GST-4"},
    {"UTC+3 莫斯科", "MSK-3"},
    {"UTC+2 开罗雅典", "EET-2"},
    {"UTC+1 中欧", "CET-1"},
    {"UTC+0 伦敦", "GMT0"},
    {"UTC0 协调世界时", "UTC0"},
    {"UTC-5 美东", "EST5"},
    {"UTC-6 美中", "CST6"},
    {"UTC-7 美西山地", "MST7"},
    {"UTC-8 美西", "PST8"},
};

const int FrameSettings::kTimeZoneCount =
    static_cast<int>(sizeof(FrameSettings::kTimeZoneList) /
                     sizeof(FrameSettings::kTimeZoneList[0]));
const int FrameSettings::kDefaultTimeZoneIndex = 0;

std::string FrameSettings::GetTz() {
    Settings nvs(kNamespace, false);
    std::string tz = nvs.GetString(kKeyTz, kDefaultTz);
    if (tz.empty()) {
        tz = kDefaultTz;
    }
    return tz;
}

void FrameSettings::SetTz(const std::string& tz) {
    if (tz.empty()) {
        ESP_LOGW(kTag, "SetTz: empty tz ignored");
        return;
    }
    Settings nvs(kNamespace, true);
    nvs.SetString(kKeyTz, tz);
    ESP_LOGI(kTag, "Timezone saved: %s", tz.c_str());
}

int FrameSettings::GetTzIndex() {
    const std::string tz = GetTz();
    for (int i = 0; i < kTimeZoneCount; ++i) {
        if (tz == kTimeZoneList[i].posix) {
            return i;
        }
    }
    return kDefaultTimeZoneIndex;
}

FrameSettings::AutoRefreshConfig FrameSettings::GetAutoRefresh() {
    Settings nvs(kNamespace, false);
    AutoRefreshConfig cfg;
    cfg.mode = nvs.GetInt(kKeyRefreshMode, kRefreshModeFixedTime);
    cfg.interval = nvs.GetInt(kKeyRefreshInterval, 24);
    cfg.unit = nvs.GetInt(kKeyRefreshUnit, kUnitHours);
    cfg.minutes_of_day = nvs.GetInt(kKeyRefreshTime, 0);

    // Sanitize values persisted by older/buggy firmware versions.
    if (cfg.mode < kRefreshModeOff || cfg.mode > kRefreshModeFixedTime) {
        cfg.mode = kRefreshModeFixedTime;
    }
    if (cfg.unit != kUnitMinutes && cfg.unit != kUnitHours) {
        cfg.unit = kUnitHours;
    }
    cfg.interval = std::max(1, std::min(cfg.interval, 24 * 7 * 60));
    cfg.minutes_of_day = std::max(0, std::min(cfg.minutes_of_day, 24 * 60 - 1));
    return cfg;
}

void FrameSettings::SetAutoRefresh(const AutoRefreshConfig& cfg) {
    AutoRefreshConfig sanitized = cfg;
    if (sanitized.mode < kRefreshModeOff || sanitized.mode > kRefreshModeFixedTime) {
        sanitized.mode = kRefreshModeFixedTime;
    }
    if (sanitized.unit != kUnitMinutes && sanitized.unit != kUnitHours) {
        sanitized.unit = kUnitHours;
    }
    sanitized.interval = std::max(1, std::min(sanitized.interval, 24 * 7 * 60));
    sanitized.minutes_of_day =
        std::max(0, std::min(sanitized.minutes_of_day, 24 * 60 - 1));

    Settings nvs(kNamespace, true);
    nvs.SetInt(kKeyRefreshMode, sanitized.mode);
    nvs.SetInt(kKeyRefreshInterval, sanitized.interval);
    nvs.SetInt(kKeyRefreshUnit, sanitized.unit);
    nvs.SetInt(kKeyRefreshTime, sanitized.minutes_of_day);
    ESP_LOGI(kTag, "Auto refresh saved: mode=%d interval=%d unit=%d time=%d",
             sanitized.mode, sanitized.interval, sanitized.unit,
             sanitized.minutes_of_day);
}

int64_t FrameSettings::MicrosUntilNextRefresh(const AutoRefreshConfig& cfg,
                                              time_t now) {
    if (cfg.mode == kRefreshModeOff) {
        return -1;
    }

    if (cfg.mode == kRefreshModeInterval) {
        const int64_t seconds =
            static_cast<int64_t>(cfg.interval) * (cfg.unit == kUnitMinutes ? 60 : 3600);
        int64_t delay_s = seconds;
        ESP_LOGI(kTag, "interval refresh in %lld s", (long long)delay_s);
        return std::min<int64_t>(delay_s * 1000LL * 1000LL, kMaxRefreshIntervalUs);
    }

    // Fixed-time mode: next local HH:MM from now, via mktime normalization.
    struct tm tmv;
    localtime_r(&now, &tmv);
    if (tmv.tm_year <= 100) {
        // SNTP not synced yet: retry in 24h so the wake still lands near the
        // configured time instead of spinning.
        ESP_LOGW(kTag, "clock not synced; scheduling next refresh in 24h");
        return 24LL * 60 * 60 * 1000 * 1000;
    }

    struct tm next = tmv;
    next.tm_hour = cfg.minutes_of_day / 60;
    next.tm_min = cfg.minutes_of_day % 60;
    next.tm_sec = 0;
    time_t next_time = mktime(&next);  // normalizes past times to tomorrow
    int64_t delay_s = static_cast<int64_t>(next_time - now);
    if (delay_s < 0) {
        // mktime already normalized, but guard anyway.
        next.tm_mday += 1;
        next_time = mktime(&next);
        delay_s = static_cast<int64_t>(next_time - now);
    }
    if (delay_s < kMinRefreshDelayS) {
        delay_s = kMinRefreshDelayS;  // sanity floor: never wake within a minute
    }
    ESP_LOGI(kTag, "fixed-time refresh at %02d:%02d in %lld s (%.1f h)",
             cfg.minutes_of_day / 60, cfg.minutes_of_day % 60,
             (long long)delay_s, delay_s / 3600.0);
    return std::min<int64_t>(delay_s * 1000LL * 1000LL, kMaxRefreshIntervalUs);
}

void FrameSettings::FormatAutoRefresh(const AutoRefreshConfig& cfg,
                                      std::string& out) {
    char buf[32];
    switch (cfg.mode) {
        case kRefreshModeOff:
            out = "关闭";
            break;
        case kRefreshModeInterval:
            if (cfg.unit == kUnitMinutes) {
                snprintf(buf, sizeof(buf), "每 %d 分钟", cfg.interval);
            } else {
                snprintf(buf, sizeof(buf), "每 %d 小时", cfg.interval);
            }
            out = buf;
            break;
        case kRefreshModeFixedTime:
        default:
            snprintf(buf, sizeof(buf), "每天 %02d:%02d",
                     cfg.minutes_of_day / 60, cfg.minutes_of_day % 60);
            out = buf;
            break;
    }
}

void FrameSettings::FormatTzLabel(const std::string& tz, std::string& out) {
    for (int i = 0; i < kTimeZoneCount; ++i) {
        if (tz == kTimeZoneList[i].posix) {
            out = kTimeZoneList[i].label;
            return;
        }
    }
    out = tz;  // raw POSIX string as fallback
}
