/**
 * @file photo_gallery.cc
 * @brief Photo frame gallery renderer for rawdraw mode (full-screen only)
 */

// 必须最先包含：它经 custom_lcd_display.h 先拉入 lvgl.h，
// 使 font_engine.h 检测到 LV_FONT_DECLARE 而启用 LVGL 真实类型；
// 若放在 photo_gallery.h 之后会先触发 font_engine.h 的兜底定义，与 lvgl 冲突。
#include "rawdraw_ui_manager.h"
#include "photo_gallery.h"
#include "common/photo_storage.h"
#include "rawdraw/layout_utils.h"
#include "rawdraw/rawdraw.h"
#include "rawdraw/style.h"
#include "rawdraw/theme.h"

#include <esp_log.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

extern const lv_font_t SourceHanSansSC_Regular_slim;
extern const lv_font_t SourceHanSansSC_Medium_slim;
extern const lv_font_t font_zectrix_16_1;

namespace rawdraw {

namespace {

static const char* kTag = "PhotoGallery";

int BytesPerRow1bpp(int width) {
    return std::max(1, (width + 7) / 8);
}

int BytesPerRow2bpp(int width) {
    return std::max(1, (width + 3) / 4);
}

bool IsBwry2bppImage(int width, int height, uint32_t size) {
    return width > 0 && height > 0 && size >= static_cast<uint32_t>(BytesPerRow2bpp(width) * height);
}

bool IsMono1bppImage(int width, int height, uint32_t size) {
    return width > 0 && height > 0 && size >= static_cast<uint32_t>(BytesPerRow1bpp(width) * height);
}

Color ReadPhotoPixelColor(const uint8_t* data, uint32_t size, int photo_width, bool bwry2bpp,
                          int src_x, int src_y) {
    if (!data || photo_width <= 0 || src_x < 0 || src_y < 0) return BLACK;
    if (bwry2bpp) {
        const int bpr = BytesPerRow2bpp(photo_width);
        const int offset = src_y * bpr + (src_x >> 2);
        if (offset < 0 || offset >= static_cast<int>(size)) return BLACK;
        const int shift = 6 - ((src_x & 0x03) * 2);
        const uint8_t color = (data[offset] >> shift) & 0x03;
        return static_cast<Color>(color);
    }

    const int bpr = BytesPerRow1bpp(photo_width);
    const int offset = src_y * bpr + (src_x >> 3);
    if (offset < 0 || offset >= static_cast<int>(size)) return BLACK;
    const int bit = 7 - (src_x & 0x07);
    return ((data[offset] >> bit) & 0x01) != 0 ? WHITE : BLACK;
}

}  // namespace

PhotoGalleryRenderer::PhotoGalleryRenderer()
    : mode_(kFullscreenMode)
    , selected_index_(0)
    , current_photo_data_(nullptr)
    , current_photo_size_(0)
    , current_photo_width_(400)
    , current_photo_height_(300)
    , font_(&SourceHanSansSC_Regular_slim)
    , title_font_(&SourceHanSansSC_Medium_slim)
    , icon_font_(&font_zectrix_16_1) {
}

PhotoGalleryRenderer::~PhotoGalleryRenderer() {
    if (current_photo_data_) {
        free(current_photo_data_);
        current_photo_data_ = nullptr;
    }
}

void PhotoGalleryRenderer::Init(int width, int height) {
    width_ = width;
    height_ = height;
    needs_full_refresh_ = true;
    mode_ = kFullscreenMode;
    selected_index_ = 0;
    RefreshPhotoList();
    // Show the stored photo right away. Without this, a boot whose initial
    // remote fetch fails (e.g. image source unreachable) would sit on the
    // "cannot load photo" placeholder even though a photo is in storage.
    if (!photo_ids_.empty()) {
        LoadPhotoData(selected_index_);
    }
}

void PhotoGalleryRenderer::Render(uint8_t* fb, int width, int height) {
    if (!fb) return;
    RenderFullscreenMode(fb, width, height);
    needs_full_refresh_ = false;
}

bool PhotoGalleryRenderer::HandleInput(const ButtonEvent& event) {
    switch (event.type) {
        case ButtonEvent::kUpClick:
            if (selected_index_ > 0) {
                selected_index_--;
                ClampSelection();
                if (mode_ == kFullscreenMode) LoadPhotoData(selected_index_);
                needs_full_refresh_ = true;
                return true;
            }
            break;
        case ButtonEvent::kDownClick:
            if (selected_index_ < GetPhotoCount() - 1) {
                selected_index_++;
                ClampSelection();
                if (mode_ == kFullscreenMode) LoadPhotoData(selected_index_);
                needs_full_refresh_ = true;
                return true;
            }
            break;
        case ButtonEvent::kBootClick:
            // Photo frame: BOOT single click is the primary "fetch a new
            // random photo" trigger. RequestRefresh only queues work;
            // RawDrawUiManager::OnRemotePhotoStored handles the update when
            // the download/convert pipeline finishes.
            if (ui::RawDrawUiManager::RequestRemotePhotoRefresh()) {
                needs_full_refresh_ = true;
                return true;
            }
            break;
        default:
            break;
    }
    return false;
}

void PhotoGalleryRenderer::RefreshPhotoList() {
    photo_ids_.clear();

    PhotoInfo info;
    int count = photo_get_count();
    for (int i = 0; i < count && i < PHOTO_MAX_PHOTOS; i++) {
        if (photo_get_by_index(i, &info) == 0) {
            PhotoEntry entry = {};
            memcpy(entry.id, info.id, sizeof(entry.id));
            memcpy(entry.title, info.title, sizeof(entry.title));
            memcpy(entry.date, info.date, sizeof(entry.date));
            memcpy(entry.location, info.location, sizeof(entry.location));
            memcpy(entry.body, info.body, sizeof(entry.body));
            entry.width = info.width;
            entry.height = info.height;
            entry.file_size = info.file_size;
            photo_ids_.push_back(entry);
        }
    }
    ClampSelection();
    ESP_LOGI(kTag, "RefreshPhotoList: storage_count=%d visible_count=%d selected=%d",
             count, static_cast<int>(photo_ids_.size()), selected_index_);
}

void PhotoGalleryRenderer::SetSelectedIndex(int index) {
    if (photo_ids_.empty()) {
        selected_index_ = 0;
        return;
    }
    selected_index_ = std::max(0, std::min(index, static_cast<int>(photo_ids_.size()) - 1));
    if (mode_ == kFullscreenMode) {
        LoadPhotoData(selected_index_);
    }
}

bool PhotoGalleryRenderer::SetSelectedById(const char* id) {
    if (!id || id[0] == '\0') return false;
    for (int i = 0; i < static_cast<int>(photo_ids_.size()); ++i) {
        if (strcmp(photo_ids_[i].id, id) == 0) {
            SetSelectedIndex(i);
            return true;
        }
    }
    return false;
}

bool PhotoGalleryRenderer::ShowRemotePhoto() {
    RefreshPhotoList();
    if (!SetSelectedById("remote00")) {
        ESP_LOGW(kTag, "remote00 not in storage yet");
        return false;
    }
    mode_ = kFullscreenMode;
    LoadPhotoData(selected_index_);
    needs_full_refresh_ = true;
    return true;
}

void PhotoGalleryRenderer::EnterFullscreenMode() {
    if (photo_ids_.empty()) return;
    mode_ = kFullscreenMode;
    LoadPhotoData(selected_index_);
}

bool PhotoGalleryRenderer::SelectNext(bool wrap) {
    const int count = GetPhotoCount();
    if (count <= 1) return false;

    int next = selected_index_ + 1;
    if (next >= count) {
        if (!wrap) return false;
        next = 0;
    }

    SetSelectedIndex(next);
    needs_full_refresh_ = true;
    ESP_LOGI(kTag, "Slideshow next photo: %d/%d", selected_index_ + 1, count);
    return true;
}

bool PhotoGalleryRenderer::IsCurrentPhotoBwry2bpp() const {
    return IsBwry2bppImage(current_photo_width_, current_photo_height_, current_photo_size_);
}

void PhotoGalleryRenderer::RenderFullscreenMode(uint8_t* fb, int width, int height) {
    const auto& theme = ThemeManager::Get();
    DrawStyledRect(fb, width, {0, 0, width, height}, theme.Style(ThemeToken::BackgroundPrimary));

    if (photo_ids_.empty()) {
        // No photo in storage yet (e.g. before first remote fetch or after
        // deleting the last photo): show a concise hint instead of a photo.
        const char* label = "暂无照片 按BOOT获取新图";
        int tw = MeasureTextWidth(label, font_);
        DrawText(fb, width, (width - tw) / 2, height / 2, label, font_,
                 theme.ColorFor(ThemeToken::TextPrimary));
        return;
    }
    if (!current_photo_data_ || current_photo_size_ == 0) {
        const char* label = "无法加载照片";
        int tw = MeasureTextWidth(label, font_);
        DrawText(fb, width, (width - tw) / 2, height / 2, label, font_,
                 theme.ColorFor(ThemeToken::TextPrimary));
        return;
    }

    const bool bwry2bpp = IsBwry2bppImage(current_photo_width_, current_photo_height_, current_photo_size_);
    const int photo_byte_width = bwry2bpp ? BytesPerRow2bpp(current_photo_width_)
                                          : BytesPerRow1bpp(current_photo_width_);
    const int expected_rows = (bwry2bpp || IsMono1bppImage(current_photo_width_, current_photo_height_, current_photo_size_))
                                  ? std::min<int>(current_photo_height_, current_photo_size_ / photo_byte_width)
                                  : 0;
    int start_y = (height - expected_rows) / 2;
    if (start_y < 0) start_y = 0;

    const int draw_w = std::min(width, current_photo_width_);
    const int start_x = std::max(0, (width - draw_w) / 2);
    for (int row = 0; row < expected_rows && (start_y + row) < height; row++) {
        for (int tx = 0; tx < draw_w; ++tx) {
            const Color src_color = ReadPhotoPixelColor(current_photo_data_, current_photo_size_,
                                                        current_photo_width_, bwry2bpp, tx, row);
            set_pixel(fb, width, start_x + tx, start_y + row, src_color);
        }
    }

}

void PhotoGalleryRenderer::LoadPhotoData(int index) {
    if (current_photo_data_) {
        free(current_photo_data_);
        current_photo_data_ = nullptr;
    }
    current_photo_size_ = 0;

    if (index < 0 || index >= static_cast<int>(photo_ids_.size())) return;

    PhotoInfo info;
    if (photo_get_by_index(index, &info) != 0) return;

    current_photo_data_ = static_cast<uint8_t*>(malloc(info.file_size));
    if (!current_photo_data_) return;

    int bytes_read = photo_load(info.id, current_photo_data_, info.file_size);
    if (bytes_read > 0) {
        current_photo_size_ = bytes_read;
        current_photo_width_ = info.width;
        current_photo_height_ = info.height;
    } else {
        free(current_photo_data_);
        current_photo_data_ = nullptr;
    }
}

void PhotoGalleryRenderer::ClampSelection() {
    int count = GetPhotoCount();
    if (count == 0) {
        selected_index_ = 0;
    } else if (selected_index_ >= count) {
        selected_index_ = count - 1;
    } else if (selected_index_ < 0) {
        selected_index_ = 0;
    }
}

}  // namespace rawdraw
