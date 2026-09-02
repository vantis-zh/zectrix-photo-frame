/**
 * @file photo_gallery.h
 * @brief Photo gallery page renderer for rawdraw mode (photo frame only)
 *
 * Full-screen mode: Single photo at full resolution.
 * When no photo is available (or loading fails), a simple placeholder is
 * rendered instead. The former "memory card" demo layout has been removed.
 *
 * Buttons:
 * - BOOT single click: request a new remote (random) photo
 */

#ifndef RAWDRAW_PHOTO_GALLERY_H
#define RAWDRAW_PHOTO_GALLERY_H

#include "common/photo_storage.h"
#include "page_renderer.h"
#include "rawdraw/style.h"
#include <vector>
#include <string>

namespace rawdraw {

/**
 * @brief Photo gallery page renderer
 */
class PhotoGalleryRenderer : public PageRenderer {
public:
    PhotoGalleryRenderer();
    ~PhotoGalleryRenderer() override;

    // PageRenderer interface
    void Init(int width, int height) override;
    void Render(uint8_t* fb, int width, int height) override;
    bool HandleInput(const ButtonEvent& event) override;

    // Display modes (photo frame mode is always full-screen)
    enum DisplayMode {
        kFullscreenMode,  // Single photo (or placeholder when empty)
    };

    // Data interface
    void RefreshPhotoList();  // Reload from photo_storage
    int GetPhotoCount() const { return static_cast<int>(photo_ids_.size()); }
    int GetSelectedIndex() const { return selected_index_; }
    void SetSelectedIndex(int index);
    bool SetSelectedById(const char* id);
    void EnterFullscreenMode();
    bool SelectNext(bool wrap);
    bool IsFullscreenMode() const { return mode_ == kFullscreenMode; }
    bool IsCurrentPhotoBwry2bpp() const;
    const uint8_t* GetCurrentPhotoData() const { return current_photo_data_; }
    uint32_t GetCurrentPhotoSize() const { return current_photo_size_; }
    int GetCurrentPhotoWidth() const { return current_photo_width_; }
    int GetCurrentPhotoHeight() const { return current_photo_height_; }

    // Remote photo frame: show the latest remotely fetched photo
    // (id "remote00") fullscreen. Returns false when it is not in storage.
    bool ShowRemotePhoto();

private:
    struct PhotoEntry {
        char id[16];
        char title[64];
        char date[PHOTO_DATE_LEN];
        char location[PHOTO_LOCATION_LEN];
        char body[PHOTO_BODY_LEN];
        uint16_t width;
        uint16_t height;
        uint32_t file_size;
    };

    // Full-screen mode rendering
    void RenderFullscreenMode(uint8_t* fb, int width, int height);

    // Photo data loading
    void LoadPhotoData(int index);

    // Helpers
    void ClampSelection();

    // State
    DisplayMode mode_ = kFullscreenMode;
    int selected_index_ = 0;

    std::vector<PhotoEntry> photo_ids_;

    // Cached photo data for current selection (1bpp or BWRY 2bpp)
    uint8_t* current_photo_data_ = nullptr;
    uint32_t current_photo_size_ = 0;
    int current_photo_width_ = 400;
    int current_photo_height_ = 300;

    // Fonts
    const lv_font_t* font_ = nullptr;
    const lv_font_t* title_font_ = nullptr;
    const lv_font_t* icon_font_ = nullptr;
};

}  // namespace rawdraw

#endif  // RAWDRAW_PHOTO_GALLERY_H
