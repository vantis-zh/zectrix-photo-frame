#ifndef _WIFI_CONFIGURATION_AP_H_
#define _WIFI_CONFIGURATION_AP_H_

#include <string>
#include <vector>
#include <mutex>
#include <memory>
#include <functional>

#include <esp_http_server.h>
#include <esp_event.h>
#include <esp_timer.h>
#include <esp_netif.h>
#include <esp_wifi_types_generic.h>

#include "dns_server.h"
#include "sdkconfig.h"

/**
 * Frame settings exchanged between the web portal (GET/POST /frame-settings)
 * and the application layer.
 *
 * refresh_mode: 0=off 1=interval 2=fixed time.
 * refresh_interval_minutes: interval carried in minutes (5..10080); the
 * application layer converts it to the best-fit NVS unit (minutes/hours).
 * refresh_time: fixed time as minutes-of-day (0..1439).
 */
struct FrameSettingsState {
    std::string tz;
    int refresh_mode = 0;
    int refresh_interval_minutes = 1440;
    int refresh_time = 0;
    // Effective image source URL ("" = built-in default). POST semantics:
    // "" resets to the default source; otherwise must be http(s) URL with
    // optional {W}/{H} placeholders (see RemotePhotoService::SetImageUrl).
    std::string image_source;
};

/**
 * WifiConfigurationAp - WiFi configuration access point
 *
 * Creates a WiFi hotspot with a captive portal for configuring WiFi credentials.
 * Note: WiFi driver must be initialized before using this class.
 */
class WifiConfigurationAp {
public:
    WifiConfigurationAp();
    ~WifiConfigurationAp();

    // Delete copy constructor and assignment operator
    WifiConfigurationAp(const WifiConfigurationAp&) = delete;
    WifiConfigurationAp& operator=(const WifiConfigurationAp&) = delete;

    void SetSsidPrefix(const std::string &&ssid_prefix);
    void SetSsidPrefix(const std::string &ssid_prefix);
    void SetPassword(const std::string &&password);
    void SetPassword(const std::string &password);
    void SetLanguage(const std::string &&language);
    void SetLanguage(const std::string &language);
    void Start();
    void Stop();
#if !CONFIG_IDF_TARGET_ESP32P4
    void StartSmartConfig();
#endif
    bool ConnectToWifi(const std::string &ssid, const std::string &password);
    void Save(const std::string &ssid, const std::string &password);
    std::vector<wifi_ap_record_t> GetAccessPoints();
    std::string GetSsid();
    std::string GetPassword() const;
    std::string GetWebServerUrl();

    /**
     * Set callback for when exit is requested from config mode
     * This is called when user requests to exit config mode (e.g., via /exit endpoint)
     */
    void OnExitRequested(std::function<void()> callback);

    /**
     * Set callback for when the web portal requests to open the settings
     * page on the device screen (POST /device-settings endpoint).
     */
    void OnOpenDeviceSettingsRequested(std::function<void()> callback);

    /**
     * Set callback for when the web portal saves frame settings
     * (POST /frame-settings endpoint). Invoked from a deferred task after
     * the HTTP response has been sent.
     */
    void OnFrameSettingsSaveRequested(std::function<void(const FrameSettingsState&)> callback);

    /**
     * Set callback for when the web portal queries the current frame
     * settings (GET /frame-settings endpoint). Runs in the httpd handler
     * context; must be quick and thread-safe.
     */
    void OnFrameSettingsQuery(std::function<FrameSettingsState()> callback);

private:
    std::mutex mutex_;
    std::unique_ptr<DnsServer> dns_server_;
    httpd_handle_t server_ = NULL;
    EventGroupHandle_t event_group_;
    std::string ssid_prefix_;
    std::string password_;
    std::string language_;
    esp_event_handler_instance_t instance_any_id_;
    esp_event_handler_instance_t instance_got_ip_;
    esp_timer_handle_t scan_timer_ = nullptr;
    bool is_connecting_ = false;
    esp_netif_t* ap_netif_ = nullptr;
    // STA netif needed so the credential-test connection (ConnectToWifi) can run
    // DHCP. Without it, association succeeds but no IP ever arrives and every
    // submit fails with "Failed to connect to the Access Point".
    esp_netif_t* sta_netif_ = nullptr;
    bool sta_netif_owned_ = false;
    std::vector<wifi_ap_record_t> ap_records_;

    // 高级配置项
    std::string ota_url_;
    int8_t max_tx_power_;
    bool remember_bssid_;
    bool sleep_mode_;

    // Callbacks
    std::function<void()> on_exit_requested_;
    std::function<void()> on_open_device_settings_requested_;
    std::function<void(const FrameSettingsState&)> on_frame_settings_save_;
    std::function<FrameSettingsState()> on_frame_settings_query_;

    void StartAccessPoint();
    void StartWebServer();

    // Event handlers
    static void WifiEventHandler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data);
    static void IpEventHandler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data);
#if !CONFIG_IDF_TARGET_ESP32P4
    static void SmartConfigEventHandler(void* arg, esp_event_base_t event_base, 
                                      int32_t event_id, void* event_data);
    esp_event_handler_instance_t sc_event_instance_ = nullptr;
#endif
};

#endif // _WIFI_CONFIGURATION_AP_H_
