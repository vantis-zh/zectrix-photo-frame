#ifndef FRAME_SETTINGS_H
#define FRAME_SETTINGS_H

#include <cstdint>
#include <ctime>
#include <string>
#include <vector>

/**
 * FrameSettings - Persistent photo-frame settings (NVS namespace "frame")
 *
 * Stores the display timezone (POSIX TZ string, only affects local display
 * and "fixed time" scheduling semantics; SNTP always syncs in UTC) and the
 * auto-refresh schedule used by EnterScheduledSleep().
 *
 * Keys:
 *   tz             - POSIX TZ string, default "CST-8" (UTC+8)
 *   refresh_mode   - 0=off 1=interval 2=fixed time (default 2 keeps the
 *                    existing "daily midnight" behaviour)
 *   refresh_interval - interval value (default 24)
 *   refresh_unit   - 0=minutes 1=hours (default 1)
 *   refresh_time   - fixed time as minutes-of-day 0..1439 (default 0 =
 *                    midnight, backwards compatible)
 */
class FrameSettings {
public:
    struct AutoRefreshConfig {
        int mode = kRefreshModeFixedTime;
        int interval = 24;
        int unit = kUnitHours;
        int minutes_of_day = 0;
    };

    static constexpr int kRefreshModeOff = 0;
    static constexpr int kRefreshModeInterval = 1;
    static constexpr int kRefreshModeFixedTime = 2;
    static constexpr int kUnitMinutes = 0;
    static constexpr int kUnitHours = 1;

    // Timezone list (display label + POSIX TZ string). Size of the list.
    struct TimeZoneEntry {
        const char* label;  // e.g. "UTC+8 中国"
        const char* posix;  // e.g. "CST-8"
    };

    static const TimeZoneEntry kTimeZoneList[];
    static const int kTimeZoneCount;
    // Index of the default timezone ("UTC+8 中国") inside kTimeZoneList.
    static const int kDefaultTimeZoneIndex;

    static std::string GetTz();
    static void SetTz(const std::string& tz);
    // Convenience: index into kTimeZoneList whose posix matches tz; falls
    // back to kDefaultTimeZoneIndex when not found.
    static int GetTzIndex();

    static AutoRefreshConfig GetAutoRefresh();
    static void SetAutoRefresh(const AutoRefreshConfig& cfg);

    /**
     * @brief Microseconds until the next auto refresh from `now`.
     *
     * mode=kRefreshModeOff: returns -1 (never wake by timer).
     * mode=kRefreshModeInterval: interval * unit (capped at 7 days).
     * mode=kRefreshModeFixedTime: next local HH:MM computed via localtime/
     * mktime (timezone must be applied with setenv("TZ",...)+tzset() before
     * calling). Falls back to 24h while SNTP is not synced, and never
     * returns less than 60s.
     */
    static int64_t MicrosUntilNextRefresh(const AutoRefreshConfig& cfg, time_t now);

    // Human-readable labels for the settings UI.
    // FormatAutoRefresh: "关闭" / "每 30 分钟" / "每 12 小时" / "每天 08:30"
    static void FormatAutoRefresh(const AutoRefreshConfig& cfg, std::string& out);
    // FormatTzLabel: label for a POSIX TZ string, "TZ字符串" fallback.
    static void FormatTzLabel(const std::string& tz, std::string& out);
};

#endif  // FRAME_SETTINGS_H
