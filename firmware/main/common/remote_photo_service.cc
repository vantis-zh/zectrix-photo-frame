/**
 * @file remote_photo_service.cc
 * @brief Random remote photo -> BWRY 2bpp -> photo_storage pipeline.
 *
 * Pipeline per refresh:
 *   1. Read image URL template from NVS (default loremflickr 400x300).
 *   2. esp_http_client GET, stream JPEG into a PSRAM buffer (cap 1.5MB).
 *   3. esp_new_jpeg (v0.6.x API) decode to RGB888.
 *   4. Center-crop to 4:3, then Floyd-Steinberg dither to the BWRY palette
 *      (algorithm ported from docs/inkscreen_image_converter.js).
 *   5. Save as photo id "remote00" via photo_storage, replacing the previous
 *      remote photo so storage stays bounded.
 *   6. Call the state callback; the UI side decides when to redraw (must
 *      happen on UI thread via RequestActivePageRefresh, not here).
 *
 * Concurrency: RequestRefresh() just gives a semaphore; all heavy work runs
 * sequentially on the dedicated worker task.
 */

#include "remote_photo_service.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <esp_http_client.h>
#include <esp_crt_bundle.h>

#include <esp_jpeg_dec.h>
#include <esp_jpeg_common.h>

#include <esp_wifi.h>

#include "wifi_manager.h"
#include "settings.h"
#include "photo_storage.h"

#include <string.h>
#include <strings.h>   // strcasecmp (Location header match)
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static const char* kTag = "RemotePhoto";

namespace {

// Screen geometry (ZecTrix 4.2" BWRY panel)
constexpr int kScreenWidth = 400;
constexpr int kScreenHeight = 300;
constexpr size_t kImage2bppSize = kScreenWidth * kScreenHeight * 2 / 8;  // 30000

// Download cap. loremflickr 400x300 jpegs are ~20-60KB but allow redirects
// to bigger originals; PSRAM is 8MB so 1.5MB cap is safe.
constexpr size_t kMaxJpegSize = 1536 * 1024;

// Max redirect hops we follow by hand (image hosts commonly chain 1-2).
constexpr int kMaxRedirects = 5;

bool IsRedirectStatus(int status) {
    return status == 301 || status == 302 || status == 303 ||
           status == 307 || status == 308;
}

// Resolve a possibly-relative redirect Location against the URL it came from.
// esp_http_client refuses to auto-follow a relative Location from an https://
// origin (ESP_ERR_HTTP_REDIRECT_DOWNGRADE), so we build the absolute URL
// ourselves and re-issue the request.
std::string ResolveUrl(const std::string& base, const std::string& loc) {
    if (loc.empty()) {
        return std::string();
    }
    if (loc.rfind("http://", 0) == 0 || loc.rfind("https://", 0) == 0) {
        return loc;  // already absolute
    }
    size_t scheme_end = base.find("://");
    if (scheme_end == std::string::npos) {
        return loc;  // malformed base, nothing sensible to do
    }
    const std::string scheme = base.substr(0, scheme_end + 3);  // "https://"
    const size_t host_start = scheme_end + 3;

    if (loc.rfind("//", 0) == 0) {  // protocol-relative //host/path
        return scheme + loc.substr(2);
    }
    if (loc[0] == '/') {  // absolute path on same host
        size_t host_end = base.find('/', host_start);
        std::string host = (host_end == std::string::npos)
                               ? base.substr(host_start)
                               : base.substr(host_start, host_end - host_start);
        return scheme + host + loc;
    }
    // Relative to the current path: replace the last path segment.
    size_t last_slash = base.rfind('/');
    if (last_slash == std::string::npos || last_slash < host_start) {
        return base + "/" + loc;
    }
    return base.substr(0, last_slash + 1) + loc;
}

// NVS persistence (Settings class wraps nvs API)
constexpr const char* kNvsNamespace = "remote_photo";
constexpr const char* kNvsKeyUrl = "remote_img_url";

// Default source: random photo at screen aspect. Replaceable at runtime via
// SetImageUrl / NVS ("remote_photo" / "remote_img_url"), e.g. later point it
// at the FnOS bridge. {W}/{H} placeholders are substituted when present.
constexpr const char* kDefaultUrlTemplate =
    "https://loremflickr.com/400/300";

// Photo id in photo_storage for the remote photo (rotates in place)
constexpr const char* kRemotePhotoId = "remote00";

// Worker task
constexpr size_t kWorkerStackSize = 12 * 1024;
constexpr UBaseType_t kWorkerPriority = 5;

// 24 hours auto refresh
constexpr uint64_t kRefreshPeriodUs = 24ULL * 60 * 60 * 1000 * 1000;

// BWRY palette and the wire-format index mapping (identical to the JS
// converter in docs/inkscreen_image_converter.js):
//   nearest-neighbor palette: 0=black 1=white 2=red 3=yellow
//   wire index:               black=0 white=1 yellow=2 red=3
struct Rgb { uint8_t r, g, b; };
constexpr Rgb kPalette[4] = {
    {0, 0, 0},        // nearest-idx 0
    {255, 255, 255},  // nearest-idx 1
    {255, 0, 0},      // nearest-idx 2 (red)
    {255, 255, 0},    // nearest-idx 3 (yellow)
};
constexpr uint8_t kNearestToWire[4] = {0, 1, 3, 2};

int BytesPerRow2bpp(int width) {
    return (width + 3) / 4;
}

}  // namespace

RemotePhotoService& RemotePhotoService::GetInstance() {
    static RemotePhotoService instance;
    return instance;
}

std::string RemotePhotoService::GetImageUrl() {
    Settings s(kNvsNamespace, false);
    std::string url = s.GetString(kNvsKeyUrl, "");
    if (url.empty()) {
        url = kDefaultUrlTemplate;
    }
    return url;
}

void RemotePhotoService::SetImageUrl(const std::string& url) {
    Settings s(kNvsNamespace, true);
    if (url.empty()) {
        s.EraseKey(kNvsKeyUrl);  // back to default
    } else {
        s.SetString(kNvsKeyUrl, url);
    }
}

bool RemotePhotoService::RequestRefresh(const char* reason) {
    if (worker_sem_ == nullptr) {
        ESP_LOGE(kTag, "not started, ignore request (%s)", reason ? reason : "?");
        return false;
    }
    if (state_mutex_ != nullptr) {
        if (xSemaphoreTake(static_cast<SemaphoreHandle_t>(state_mutex_),
                           pdMS_TO_TICKS(100)) == pdTRUE) {
            pending_reason_ = reason ? reason : "manual";
            xSemaphoreGive(static_cast<SemaphoreHandle_t>(state_mutex_));
        }
    }
    xSemaphoreGive(static_cast<SemaphoreHandle_t>(worker_sem_));
    return true;
}

void RemotePhotoService::TimerCallback(void*) {
    ESP_LOGI(kTag, "24h auto refresh");
    GetInstance().RequestRefresh("24h-timer");
}

void RemotePhotoService::WorkerTaskEntry(void* arg) {
    auto* self = static_cast<RemotePhotoService*>(arg);
    self->WorkerLoop();
}

void RemotePhotoService::WorkerLoop() {
    auto* sem = static_cast<SemaphoreHandle_t>(worker_sem_);
    for (;;) {
        if (xSemaphoreTake(sem, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        FetchAndStoreOnce();
    }
}

bool RemotePhotoService::FetchAndStoreOnce() {
    auto set_state = [this](State st, const char* msg) {
        state_ = st;
        if (msg) last_message_ = msg;
        if (state_cb_) state_cb_(st, last_message_);
    };

    // --------------------------------------------------------------
    // 0. WiFi must be up
    // --------------------------------------------------------------
    if (!WifiManager::GetInstance().IsConnected()) {
        set_state(kError, "wifi not connected");
        return false;
    }

    set_state(kDownloading, "downloading");

    // --------------------------------------------------------------
    // 1. Build URL (substitute {W}/{H})
    // --------------------------------------------------------------
    std::string url = GetImageUrl();
    {
        size_t pos;
        const std::string w_tok = "{W}";
        const std::string h_tok = "{H}";
        while ((pos = url.find(w_tok)) != std::string::npos) {
            url.replace(pos, w_tok.size(), std::to_string(kScreenWidth));
        }
        while ((pos = url.find(h_tok)) != std::string::npos) {
            url.replace(pos, h_tok.size(), std::to_string(kScreenHeight));
        }
    }
    ESP_LOGI(kTag, "fetch: %s", url.c_str());

    // --------------------------------------------------------------
    // 2. Download into PSRAM
    // --------------------------------------------------------------
    uint8_t* jpeg_buf =
        static_cast<uint8_t*>(heap_caps_malloc(kMaxJpegSize,
                                               MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (jpeg_buf == nullptr) {
        // Fall back to internal RAM if PSRAM not available
        jpeg_buf = static_cast<uint8_t*>(malloc(kMaxJpegSize));
    }
    if (jpeg_buf == nullptr) {
        set_state(kError, "jpeg buf alloc failed");
        return false;
    }
    size_t jpeg_len = 0;

    {
        struct StreamCtx {
            uint8_t* buf;
            size_t cap;
            size_t len;
            bool overflow;
            std::string location;  // captured Location header, if any
        } ctx = {jpeg_buf, kMaxJpegSize, 0, false, std::string()};

        auto event_handler = +[](esp_http_client_event_t* evt) -> esp_err_t {
            auto* c = static_cast<StreamCtx*>(evt->user_data);
            if (c == nullptr) {
                return ESP_OK;
            }
            if (evt->event_id == HTTP_EVENT_ON_DATA) {
                if (c->len + (size_t)evt->data_len > c->cap) {
                    c->overflow = true;
                    return ESP_FAIL;  // abort transfer
                }
                memcpy(c->buf + c->len, evt->data, evt->data_len);
                c->len += (size_t)evt->data_len;
            } else if (evt->event_id == HTTP_EVENT_ON_HEADER) {
                // NB: IDF dispatches this event with data_len == 0
                // (esp_http_client.c: http_on_header_event passes NULL, 0),
                // so the value length must come from strlen(), not data_len.
                if (evt->header_key != nullptr && evt->header_value != nullptr &&
                    strcasecmp(evt->header_key, "Location") == 0) {
                    c->location.assign(evt->header_value);
                }
            }
            return ESP_OK;
        };

        std::string current_url = url;
        esp_err_t err = ESP_FAIL;
        int status = 0;

        for (int hop = 0; hop < kMaxRedirects; ++hop) {
            ctx.len = 0;
            ctx.overflow = false;
            ctx.location.clear();

            esp_http_client_config_t config = {};
            config.url = current_url.c_str();
            config.method = HTTP_METHOD_GET;
            config.timeout_ms = 20000;
            config.buffer_size = 4096;
            config.user_data = &ctx;
            config.event_handler = event_handler;
            // Followed by hand below — see ResolveUrl(). Auto-follow rejects
            // relative Locations with ESP_ERR_HTTP_REDIRECT_DOWNGRADE.
            config.disable_auto_redirect = true;
            // HTTPS: esp-tls refuses to set up a TLS session unless one server
            // verification option is set, otherwise it fails with
            // "No server verification option set" (0x8017). Use the built-in
            // certificate bundle (CONFIG_MBEDTLS_CERTIFICATE_BUNDLE=y).
            config.crt_bundle_attach = esp_crt_bundle_attach;

            esp_http_client_handle_t client = esp_http_client_init(&config);
            if (client == nullptr) {
                free(jpeg_buf);
                set_state(kError, "http init failed");
                return false;
            }
            err = esp_http_client_perform(client);
            status = esp_http_client_get_status_code(client);
            esp_http_client_cleanup(client);

            if (err == ESP_OK && IsRedirectStatus(status) && !ctx.location.empty()) {
                std::string next = ResolveUrl(current_url, ctx.location);
                if (next.empty() || next == current_url) {
                    break;
                }
                ESP_LOGI(kTag, "redirect %d -> %s", status, next.c_str());
                current_url = next;
                continue;
            }
            break;
        }

        jpeg_len = ctx.len;
        if (err != ESP_OK || status != 200 || ctx.overflow || jpeg_len == 0) {
            free(jpeg_buf);
            ESP_LOGE(kTag, "download failed: err=%s status=%d len=%u",
                     esp_err_to_name(err), status, (unsigned)jpeg_len);
            set_state(kError, "download failed");
            return false;
        }
        ESP_LOGI(kTag, "downloaded %u bytes", (unsigned)jpeg_len);
    }

    set_state(kDecoding, "decoding");

    // --------------------------------------------------------------
    // 3. Decode (esp_new_jpeg v0.6.x API, per test_app example)
    // --------------------------------------------------------------
    jpeg_dec_config_t dec_cfg = DEFAULT_JPEG_DEC_CONFIG();
    dec_cfg.output_type = JPEG_PIXEL_FORMAT_RGB888;

    jpeg_dec_handle_t jpeg_dec = nullptr;
    jpeg_dec_io_t io = {};
    jpeg_dec_header_info_t header = {};
    uint8_t* rgb_buf = nullptr;
    bool ok = false;

    do {
        if (jpeg_dec_open(&dec_cfg, &jpeg_dec) != JPEG_ERR_OK) {
            set_state(kError, "jpeg open failed");
            break;
        }

        io.inbuf = jpeg_buf;
        io.inbuf_len = static_cast<int>(jpeg_len);

        if (jpeg_dec_parse_header(jpeg_dec, &io, &header) != JPEG_ERR_OK) {
            set_state(kError, "jpeg header failed");
            break;
        }
        const int src_w = header.width;
        const int src_h = header.height;
        ESP_LOGI(kTag, "jpeg %dx%d", src_w, src_h);

        const int out_len = src_w * src_h * 3;
        // 16-byte aligned buffer (requirement for esp32-s3 SIMD path)
        rgb_buf = static_cast<uint8_t*>(jpeg_calloc_align(out_len, 16));
        if (rgb_buf == nullptr) {
            set_state(kError, "rgb alloc failed");
            break;
        }
        io.outbuf = rgb_buf;

        if (jpeg_dec_process(jpeg_dec, &io) != JPEG_ERR_OK) {
            set_state(kError, "jpeg decode failed");
            break;
        }
        ok = true;
    } while (false);

    jpeg_dec_close(jpeg_dec);
    free(jpeg_buf);
    jpeg_buf = nullptr;

    if (!ok) {
        if (rgb_buf) jpeg_free_align(rgb_buf);
        return false;
    }

    const int src_w = header.width;
    const int src_h = header.height;

    set_state(kSaving, "dithering");

    // --------------------------------------------------------------
    // 4. Center-crop to 4:3 and Floyd-Steinberg dither to BWRY 2bpp
    // --------------------------------------------------------------
    uint8_t* out2bpp = static_cast<uint8_t*>(malloc(kImage2bppSize));
    if (out2bpp == nullptr) {
        jpeg_free_align(rgb_buf);
        set_state(kError, "out alloc failed");
        return false;
    }
    memset(out2bpp, 0, kImage2bppSize);

    // Source crop rect (aspect-fit to 400:300, centered)
    const float src_aspect = (float)src_w / (float)src_h;
    const float dst_aspect = (float)kScreenWidth / (float)kScreenHeight;
    int crop_w = src_w;
    int crop_h = src_h;
    if (src_aspect > dst_aspect) {
        crop_w = (int)((float)src_h * dst_aspect);
    } else if (src_aspect < dst_aspect) {
        crop_h = (int)((float)src_w / dst_aspect);
    }
    const int crop_x = (src_w - crop_w) / 2;
    const int crop_y = (src_h - crop_h) / 2;

    // Per-row error buffers (FS needs prev/next rows; width+2 slack)
    float* err_rows[2][3];
    for (int r = 0; r < 2; ++r) {
        for (int c = 0; c < 3; ++c) {
            err_rows[r][c] =
                static_cast<float*>(calloc(crop_w + 2, sizeof(float)));
        }
    }
    bool err_ok = true;
    for (int r = 0; r < 2 && err_ok; ++r) {
        for (int c = 0; c < 3 && err_ok; ++c) {
            if (err_rows[r][c] == nullptr) err_ok = false;
        }
    }
    if (!err_ok) {
        for (int r = 0; r < 2; ++r) {
            for (int c = 0; c < 3; ++c) free(err_rows[r][c]);
        }
        free(out2bpp);
        jpeg_free_align(rgb_buf);
        set_state(kError, "err buf alloc failed");
        return false;
    }

    const int bpr = BytesPerRow2bpp(kScreenWidth);

    for (int y = 0; y < kScreenHeight; ++y) {
        const int sy = crop_y + (y * crop_h) / kScreenHeight;
        float* err_r = err_rows[0][0];
        float* err_g = err_rows[0][1];
        float* err_b = err_rows[0][2];
        float* next_r = err_rows[1][0];
        float* next_g = err_rows[1][1];
        float* next_b = err_rows[1][2];

        for (int x = 0; x < kScreenWidth; ++x) {
            const int sx = crop_x + (x * crop_w) / kScreenWidth;
            const uint8_t* px =
                rgb_buf + ((size_t)sy * src_w + sx) * 3;

            // Small brightness lift; the EPD panel reflects noticeably less
            // light than a LCD photo, so lift midtones before quantizing.
            int rr = px[0] + 16;
            int gg = px[1] + 16;
            int bb = px[2] + 16;
            if (rr > 255) rr = 255;
            if (gg > 255) gg = 255;
            if (bb > 255) bb = 255;

            const float fr = (float)rr + err_r[x + 1];
            const float fg = (float)gg + err_g[x + 1];
            const float fb = (float)bb + err_b[x + 1];

            int best = 0;
            float best_d = 1e30f;
            for (int k = 0; k < 4; ++k) {
                const float dr = fr - (float)kPalette[k].r;
                const float dg = fg - (float)kPalette[k].g;
                const float db = fb - (float)kPalette[k].b;
                const float d = 0.299f * dr * dr +
                                0.587f * dg * dg +
                                0.114f * db * db;
                if (d < best_d) {
                    best_d = d;
                    best = k;
                }
            }

            const float er = fr - (float)kPalette[best].r;
            const float eg = fg - (float)kPalette[best].g;
            const float eb = fb - (float)kPalette[best].b;

            // Floyd-Steinberg: 7/16 right, 3/16 below-left, 5/16 below,
            // 1/16 below-right (same as the JS converter)
            err_r[x + 2] += er * 7.0f / 16.0f;
            err_g[x + 2] += eg * 7.0f / 16.0f;
            err_b[x + 2] += eb * 7.0f / 16.0f;

            next_r[x]     += er * 3.0f / 16.0f;
            next_g[x]     += eg * 3.0f / 16.0f;
            next_b[x]     += eb * 3.0f / 16.0f;

            next_r[x + 1] += er * 5.0f / 16.0f;
            next_g[x + 1] += eg * 5.0f / 16.0f;
            next_b[x + 1] += eb * 5.0f / 16.0f;

            next_r[x + 2] += er * 1.0f / 16.0f;
            next_g[x + 2] += eg * 1.0f / 16.0f;
            next_b[x + 2] += eb * 1.0f / 16.0f;

            const uint8_t wire = kNearestToWire[best];
            out2bpp[y * bpr + (x >> 2)] |=
                wire << (6 - ((x & 3) * 2));
        }

        // Rotate error rows and clear the new "next" row
        for (int c = 0; c < 3; ++c) {
            float* t = err_rows[0][c];
            err_rows[0][c] = err_rows[1][c];
            err_rows[1][c] = t;
            memset(err_rows[1][c], 0, (crop_w + 2) * sizeof(float));
        }
    }

    for (int r = 0; r < 2; ++r) {
        for (int c = 0; c < 3; ++c) free(err_rows[r][c]);
    }
    jpeg_free_align(rgb_buf);
    rgb_buf = nullptr;

    // --------------------------------------------------------------
    // 5. Persist: replace previous remote photo
    // --------------------------------------------------------------
    if (photo_exists(kRemotePhotoId)) {
        photo_delete(kRemotePhotoId);
    }

    PhotoInfo info = {};
    strncpy(info.id, kRemotePhotoId, sizeof(info.id) - 1);
    snprintf(info.title, sizeof(info.title), "Remote photo");
    snprintf(info.body, sizeof(info.body), "fetched from remote source");
    {
        time_t now = time(nullptr);
        struct tm tmv;
        localtime_r(&now, &tmv);
        if (tmv.tm_year > 100) {
            strftime(info.date, sizeof(info.date), "%Y-%m-%d", &tmv);
        }
    }
    info.width = kScreenWidth;
    info.height = kScreenHeight;
    info.file_size = kImage2bppSize;
    info.timestamp = (uint32_t)time(nullptr);

    if (photo_save(&info, out2bpp) != 0) {
        free(out2bpp);
        set_state(kError, "save failed");
        return false;
    }
    free(out2bpp);

    ESP_LOGI(kTag, "remote photo stored (%s)", kRemotePhotoId);
    set_state(kDone, "ok");
    return true;
}

void RemotePhotoService::Start() {
    if (worker_sem_ != nullptr) {
        return;
    }
    worker_sem_ = xSemaphoreCreateCounting(4, 0);
    state_mutex_ = xSemaphoreCreateMutex();

    if (xTaskCreate(WorkerTaskEntry, "remote_photo", kWorkerStackSize,
                    this, kWorkerPriority, nullptr) != pdPASS) {
        ESP_LOGE(kTag, "failed to create worker task");
        return;
    }

    esp_timer_handle_t timer = nullptr;
    const esp_timer_create_args_t timer_args = {
        .callback = &RemotePhotoService::TimerCallback,
        .arg = nullptr,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "remote_photo_t",
        .skip_unhandled_events = false,
    };
    if (esp_timer_create(&timer_args, &timer) == ESP_OK) {
        esp_timer_start_periodic(timer, kRefreshPeriodUs);
    } else {
        ESP_LOGE(kTag, "failed to create refresh timer");
    }

    ESP_LOGI(kTag, "started, url=%s", GetImageUrl().c_str());
}
