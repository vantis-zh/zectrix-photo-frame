/**
 * @file remote_photo_service.h
 * @brief Fetch random photo from a configurable remote source, convert to
 *        BWRY 2bpp (400x300), store via photo_storage.
 *
 * Simple version of the "photo frame" feature:
 * - Button press or 24h timer triggers a fetch.
 * - Image source is a URL template, persisted in NVS
 *   ("remote_photo"/"remote_img_url"), swappable without reflashing
 *   (e.g. later switch from loremflickr to the FnOS NAS bridge).
 * - Fetched JPEG is decoded with esp_new_jpeg and dithered to the BWRY
 *   palette with Floyd-Steinberg (ported from
 *   docs/inkscreen_image_converter.js).
 *
 * Threading: RequestRefresh() is task-safe (semaphore). Heavy work runs on
 * the internal worker task; the state callback fires on the worker task, so
 * UI updates triggered from it must marshal back to the UI thread.
 */

#ifndef REMOTE_PHOTO_SERVICE_H
#define REMOTE_PHOTO_SERVICE_H

#include <stdint.h>
#include <stdbool.h>
#include <string>
#include <functional>

class RemotePhotoService {
public:
    enum State {
        kIdle = 0,
        kDownloading,
        kDecoding,
        kSaving,
        kDone,
        kError,
    };

    using StateCallback = std::function<void(State state, const std::string& message)>;

    static RemotePhotoService& GetInstance();

    // Called once from Application::Start after WiFi/UI init.
    // Spawns the worker task and the 24h periodic timer.
    void Start();

    // Queue a fetch cycle. Non-blocking, task-safe. Ignored when the
    // service has not been started yet.
    bool RequestRefresh(const char* reason = "manual");

    // Runtime-configurable source URL template. "{W}"/"{H}" placeholders
    // are replaced with screen size. Empty string restores the default.
    static std::string GetImageUrl();
    static void SetImageUrl(const std::string& url);

    State state() const { return state_; }
    const char* last_message() const { return last_message_.c_str(); }

    // Callback fires on the worker task after each state change.
    void SetStateCallback(StateCallback cb) { state_cb_ = std::move(cb); }

    RemotePhotoService(const RemotePhotoService&) = delete;
    RemotePhotoService& operator=(const RemotePhotoService&) = delete;

private:
    RemotePhotoService() = default;
    ~RemotePhotoService() = default;

    static void WorkerTaskEntry(void* arg);
    void WorkerLoop();
    static void TimerCallback(void* arg);
    bool FetchAndStoreOnce();

    // Best-effort status, read from other tasks (torn reads acceptable
    // in the simple version: strings are only replaced on completion)
    State state_ = kIdle;
    std::string last_message_;
    StateCallback state_cb_;

    void* worker_sem_ = nullptr;   // SemaphoreHandle_t
    void* state_mutex_ = nullptr;  // SemaphoreHandle_t
    std::string pending_reason_;
};

#endif  // REMOTE_PHOTO_SERVICE_H

