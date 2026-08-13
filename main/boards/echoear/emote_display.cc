#include "emote_display.h"

#include <cstring>
#include <cmath>
#include <memory>
#include <unordered_map>
#include <tuple>
#include <utility>
#include <esp_log.h>
#include <esp_lcd_panel_io.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <sys/time.h>
#include <time.h>

#include "display/lcd_display.h"
#include "application.h"
#include "expression_director.h"
#include "mmap_generate_emoji_normal.h"
#include "config.h"
#include "gfx.h"

namespace anim {

static const char* TAG = "emoji";

// UI element management
static gfx_obj_t* obj_label_tips = nullptr;
static gfx_obj_t* obj_label_time = nullptr;
static gfx_obj_t* obj_anim_eye = nullptr;
static gfx_obj_t* obj_anim_mic = nullptr;
static gfx_obj_t* obj_img_icon = nullptr;
static gfx_obj_t* obj_img_music_background = nullptr;
static gfx_obj_t* obj_img_music_disc = nullptr;
static gfx_obj_t* obj_label_music_title = nullptr;
static gfx_obj_t* obj_label_music_artist = nullptr;
static gfx_obj_t* obj_label_music_previous = nullptr;
static gfx_obj_t* obj_label_music_current = nullptr;
static gfx_obj_t* obj_label_music_next = nullptr;
static gfx_obj_t* obj_label_music_progress = nullptr;
static gfx_image_dsc_t icon_img_dsc;
static gfx_font_t font_tips = nullptr;
static gfx_font_t font_time = nullptr;

// Track current icon to determine when to show time
static int current_icon_type = MMAP_EMOJI_NORMAL_ICON_BATTERY_BIN;

enum class UIDisplayMode : uint8_t {
    SHOW_NONE = 0,      // Immersive eyes: no status overlay
    SHOW_ANIM_TOP = 1,  // Show obj_anim_mic
    SHOW_TIME = 2,      // Show obj_label_time
    SHOW_TIPS = 3       // Show obj_label_tips
};

static UIDisplayMode current_ui_display_mode = UIDisplayMode::SHOW_NONE;

static void SetUIDisplayMode(UIDisplayMode mode)
{
    current_ui_display_mode = mode;
    if (obj_anim_mic) gfx_obj_set_visible(obj_anim_mic, false);
    if (obj_label_time) gfx_obj_set_visible(obj_label_time, false);
    if (obj_label_tips) gfx_obj_set_visible(obj_label_tips, false);
    if (obj_img_icon) gfx_obj_set_visible(obj_img_icon, mode != UIDisplayMode::SHOW_NONE);

    // Show the selected control
    switch (mode) {
    case UIDisplayMode::SHOW_NONE:
        break;
    case UIDisplayMode::SHOW_ANIM_TOP:
        gfx_obj_set_visible(obj_anim_mic, true);
        break;
    case UIDisplayMode::SHOW_TIME:
        gfx_obj_set_visible(obj_label_time, true);
        break;
    case UIDisplayMode::SHOW_TIPS:
        gfx_obj_set_visible(obj_label_tips, true);
        break;
    }
}

static void clock_tm_callback(void* user_data)
{
    auto* engine = static_cast<EmoteEngine*>(user_data);
    if (engine && engine->IsMusicSceneActive()) {
        return;
    }
    // Do not let the periodic clock callback break immersive eye expressions.
    if (current_ui_display_mode == UIDisplayMode::SHOW_TIME &&
        current_icon_type == MMAP_EMOJI_NORMAL_ICON_BATTERY_BIN) {
        time_t now;
        struct tm timeinfo;
        time(&now);

        setenv("TZ", "GMT+0", 1);
        tzset();
        localtime_r(&now, &timeinfo);

        char time_str[6];
        snprintf(time_str, sizeof(time_str), "%02d:%02d", timeinfo.tm_hour, timeinfo.tm_min);

        gfx_label_set_text(obj_label_time, time_str);
        SetUIDisplayMode(UIDisplayMode::SHOW_TIME);
    }
}

static void InitializeAssets(mmap_assets_handle_t* assets_handle)
{
    const mmap_assets_config_t assets_cfg = {
        .partition_label = "assets_A",
        .max_files = MMAP_EMOJI_NORMAL_FILES,
        .checksum = MMAP_EMOJI_NORMAL_CHECKSUM,
        .flags = {.mmap_enable = true, .full_check = true}
    };

    mmap_assets_new(&assets_cfg, assets_handle);
}

static void InitializeGraphics(esp_lcd_panel_handle_t panel, gfx_handle_t* engine_handle)
{
    gfx_core_config_t gfx_cfg = {
        .flush_cb = EmoteEngine::OnFlush,
        .user_data = panel,
        .flags = {
            .swap = true,
            .double_buffer = true,
            .buff_dma = true,
        },
        .h_res = DISPLAY_WIDTH,
        .v_res = DISPLAY_HEIGHT,
        .fps = 30,
        .buffers = {
            .buf1 = nullptr,
            .buf2 = nullptr,
            .buf_pixels = DISPLAY_WIDTH * 16,
        },
        .task = GFX_EMOTE_INIT_CONFIG()
    };

    gfx_cfg.task.task_stack_caps = MALLOC_CAP_DEFAULT;
    gfx_cfg.task.task_affinity = 0;
    gfx_cfg.task.task_priority = 5;
    gfx_cfg.task.task_stack = 20 * 1024;

    *engine_handle = gfx_emote_init(&gfx_cfg);
}

static void InitializeEyeAnimation(gfx_handle_t engine_handle, mmap_assets_handle_t assets_handle)
{
    obj_anim_eye = gfx_anim_create(engine_handle);

    const void* anim_data = mmap_assets_get_mem(assets_handle, MMAP_EMOJI_NORMAL_NEUTRAL_EAF);
    size_t anim_size = mmap_assets_get_size(assets_handle, MMAP_EMOJI_NORMAL_NEUTRAL_EAF);

    gfx_anim_set_src(obj_anim_eye, anim_data, anim_size);

    gfx_obj_align(obj_anim_eye, GFX_ALIGN_LEFT_MID, 10, -20);
    // Eye assets are mirrored from a single-eye animation.  Automatic mirror
    // spacing keeps both the legacy 173 px AAF assets and the newer 125 px EAF
    // assets centered on the 360 px EchoEar display.
    gfx_anim_set_auto_mirror(obj_anim_eye, true);
    gfx_anim_set_segment(obj_anim_eye, 0, 0xFFFF, 20, false);
    gfx_anim_start(obj_anim_eye);
}

static gfx_font_t CreateFont(mmap_assets_handle_t assets_handle, uint16_t font_size)
{
    gfx_label_cfg_t font_cfg = {
        .name = "KaiTi.ttf",
        .mem = mmap_assets_get_mem(assets_handle, MMAP_EMOJI_NORMAL_KAITI_TTF),
        .mem_size = static_cast<size_t>(mmap_assets_get_size(assets_handle, MMAP_EMOJI_NORMAL_KAITI_TTF)),
        .font_size = font_size,
    };

    gfx_font_t font = nullptr;
    ESP_ERROR_CHECK(gfx_label_new_font(&font_cfg, &font));
    return font;
}

static void InitializeFonts(mmap_assets_handle_t assets_handle)
{
    font_tips = CreateFont(assets_handle, 20);
    font_time = CreateFont(assets_handle, 40);

    ESP_LOGI(TAG, "stack: %d", uxTaskGetStackHighWaterMark(nullptr));
}

static void InitializeLabels(gfx_handle_t engine_handle)
{
    // Initialize tips label
    obj_label_tips = gfx_label_create(engine_handle);
    gfx_obj_align(obj_label_tips, GFX_ALIGN_TOP_MID, 0, 45);
    gfx_obj_set_size(obj_label_tips, 160, 40);
    gfx_label_set_text(obj_label_tips, "启动中...");
    gfx_label_set_font(obj_label_tips, font_tips);
    gfx_label_set_color(obj_label_tips, GFX_COLOR_HEX(0xFFFFFF));
    gfx_label_set_text_align(obj_label_tips, GFX_TEXT_ALIGN_LEFT);
    gfx_label_set_long_mode(obj_label_tips, GFX_LABEL_LONG_SCROLL);
    gfx_label_set_scroll_speed(obj_label_tips, 20);
    gfx_label_set_scroll_loop(obj_label_tips, true);

    // Initialize time label
    obj_label_time = gfx_label_create(engine_handle);
    gfx_obj_align(obj_label_time, GFX_ALIGN_TOP_MID, 0, 30);
    gfx_obj_set_size(obj_label_time, 160, 50);
    gfx_label_set_text(obj_label_time, "--:--");
    gfx_label_set_font(obj_label_time, font_time);
    gfx_label_set_color(obj_label_time, GFX_COLOR_HEX(0xFFFFFF));
    gfx_label_set_text_align(obj_label_time, GFX_TEXT_ALIGN_CENTER);
}

static void InitializeMicAnimation(gfx_handle_t engine_handle, mmap_assets_handle_t assets_handle)
{
    obj_anim_mic = gfx_anim_create(engine_handle);
    gfx_obj_align(obj_anim_mic, GFX_ALIGN_TOP_MID, 0, 25);

    const void* anim_data = mmap_assets_get_mem(assets_handle, MMAP_EMOJI_NORMAL_LISTEN_EAF);
    size_t anim_size = mmap_assets_get_size(assets_handle, MMAP_EMOJI_NORMAL_LISTEN_EAF);
    gfx_anim_set_src(obj_anim_mic, anim_data, anim_size);
    gfx_anim_start(obj_anim_mic);
    gfx_obj_set_visible(obj_anim_mic, false);
}

static void InitializeIcon(gfx_handle_t engine_handle, mmap_assets_handle_t assets_handle)
{
    obj_img_icon = gfx_img_create(engine_handle);
    gfx_obj_align(obj_img_icon, GFX_ALIGN_TOP_MID, -100, 38);

    if (SetupImageDescriptor(assets_handle, &icon_img_dsc,
                             MMAP_EMOJI_NORMAL_ICON_WIFI_FAILED_BIN)) {
        gfx_img_set_src(obj_img_icon, static_cast<void*>(&icon_img_dsc));
    }
}

static void InitializeMusicUi(gfx_handle_t engine_handle)
{
    obj_img_music_disc = gfx_img_create(engine_handle);
    gfx_obj_align(obj_img_music_disc, GFX_ALIGN_CENTER, 0, -8);

    auto create_label = [engine_handle](gfx_obj_t** object, int y, int height,
                                       gfx_font_t font, gfx_color_t color) {
        *object = gfx_label_create(engine_handle);
        gfx_obj_align(*object, GFX_ALIGN_TOP_MID, 0, y);
        gfx_obj_set_size(*object, 332, height);
        gfx_label_set_font(*object, font);
        gfx_label_set_color(*object, color);
        gfx_label_set_text_align(*object, GFX_TEXT_ALIGN_CENTER);
        gfx_label_set_long_mode(*object, GFX_LABEL_LONG_SCROLL);
        gfx_label_set_scroll_speed(*object, 24);
        gfx_label_set_scroll_loop(*object, true);
        gfx_obj_set_visible(*object, false);
    };
    create_label(&obj_label_music_title, 10, 28, font_tips, GFX_COLOR_HEX(0xFFFFFF));
    create_label(&obj_label_music_artist, 38, 24, font_tips, GFX_COLOR_HEX(0xB8C2D8));
    // The 192 px disc occupies y=76..267. Keep the complete lyric stack below
    // it so dirty disc refreshes can never clip or visually cover the first row.
    create_label(&obj_label_music_previous, 272, 22, font_tips, GFX_COLOR_HEX(0x8490A8));
    create_label(&obj_label_music_current, 296, 26, font_tips, GFX_COLOR_HEX(0xFFFFFF));
    create_label(&obj_label_music_next, 324, 20, font_tips, GFX_COLOR_HEX(0x8490A8));
    create_label(&obj_label_music_progress, 345, 14, font_tips, GFX_COLOR_HEX(0x9AA6BC));
    gfx_obj_set_visible(obj_img_music_disc, false);
}

static void SetMusicObjectsVisible(bool visible)
{
    gfx_obj_set_visible(obj_img_music_background, visible);
    gfx_obj_set_visible(obj_img_music_disc, visible);
    gfx_obj_set_visible(obj_label_music_title, visible);
    gfx_obj_set_visible(obj_label_music_artist, visible);
    gfx_obj_set_visible(obj_label_music_previous, visible);
    gfx_obj_set_visible(obj_label_music_current, visible);
    gfx_obj_set_visible(obj_label_music_next, visible);
    gfx_obj_set_visible(obj_label_music_progress, visible);
}

static void RegisterCallbacks(esp_lcd_panel_io_handle_t panel_io, gfx_handle_t engine_handle)
{
    const esp_lcd_panel_io_callbacks_t cbs = {
        .on_color_trans_done = EmoteEngine::OnFlushIoReady,
    };
    esp_lcd_panel_io_register_event_callbacks(panel_io, &cbs, engine_handle);
}

bool SetupImageDescriptor(mmap_assets_handle_t assets_handle,
                          gfx_image_dsc_t* img_dsc,
                          int asset_id)
{
    if (assets_handle == nullptr || img_dsc == nullptr) {
        ESP_LOGE(TAG, "Cannot load icon asset %d: assets unavailable", asset_id);
        return false;
    }
    const void* img_data = mmap_assets_get_mem(assets_handle, asset_id);
    size_t img_size = mmap_assets_get_size(assets_handle, asset_id);
    if (img_data == nullptr || img_size <= sizeof(gfx_image_header_t)) {
        ESP_LOGE(TAG, "Cannot load icon asset %d: invalid size %u",
                 asset_id, static_cast<unsigned>(img_size));
        return false;
    }

    std::memcpy(&img_dsc->header, img_data, sizeof(gfx_image_header_t));
    img_dsc->data = static_cast<const uint8_t*>(img_data) + sizeof(gfx_image_header_t);
    img_dsc->data_size = img_size - sizeof(gfx_image_header_t);
    return true;
}

EmoteEngine::EmoteEngine(esp_lcd_panel_handle_t panel, esp_lcd_panel_io_handle_t panel_io)
{
    ESP_LOGI(TAG, "Create EmoteEngine, panel: %p, panel_io: %p", panel, panel_io);

    InitializeAssets(&assets_handle_);
    InitializeGraphics(panel, &engine_handle_);

    gfx_emote_lock(engine_handle_);
    gfx_emote_set_bg_color(engine_handle_, GFX_COLOR_HEX(0x000000));

    // Initialize all UI components
    obj_img_music_background = gfx_img_create(engine_handle_);
    gfx_obj_align(obj_img_music_background, GFX_ALIGN_TOP_LEFT, 0, 0);
    gfx_obj_set_visible(obj_img_music_background, false);
    InitializeEyeAnimation(engine_handle_, assets_handle_);
    InitializeFonts(assets_handle_);
    InitializeLabels(engine_handle_);
    InitializeMicAnimation(engine_handle_, assets_handle_);
    InitializeIcon(engine_handle_, assets_handle_);
    InitializeMusicUi(engine_handle_);

    current_icon_type = MMAP_EMOJI_NORMAL_ICON_WIFI_FAILED_BIN;
    SetUIDisplayMode(UIDisplayMode::SHOW_TIPS);

    gfx_timer_create(engine_handle_, clock_tm_callback, 1000, this);

    gfx_emote_unlock(engine_handle_);

    RegisterCallbacks(panel_io, engine_handle_);

    constexpr float center = 95.5f;
    constexpr float outer_radius = 95.0f;
    for (int y = 0; y < 192; ++y) {
        const float dy = static_cast<float>(y) - center;
        const float half_width = sqrtf(std::max(0.0f, outer_radius * outer_radius - dy * dy));
        music_disc_left_[y] = static_cast<int16_t>(std::max(0, static_cast<int>(ceilf(center - half_width))));
        music_disc_right_[y] = static_cast<int16_t>(std::min(191, static_cast<int>(floorf(center + half_width))));
    }

    const esp_timer_create_args_t rotation_timer_args = {
        .callback = MusicRotationTimer,
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "music_disc",
        .skip_unhandled_events = true,
    };
    if (esp_timer_create(&rotation_timer_args, &music_rotation_timer_) != ESP_OK) {
        music_rotation_timer_ = nullptr;
    }
    const esp_timer_create_args_t fallback_timer_args = {
        .callback = MusicFallbackTimer,
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "music_fallback",
        .skip_unhandled_events = true,
    };
    if (esp_timer_create(&fallback_timer_args, &music_fallback_timer_) != ESP_OK) {
        music_fallback_timer_ = nullptr;
    }
    const esp_timer_create_args_t release_timer_args = {
        .callback = MusicReleaseTimer,
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "music_release",
        .skip_unhandled_events = true,
    };
    if (esp_timer_create(&release_timer_args, &music_release_timer_) != ESP_OK) {
        music_release_timer_ = nullptr;
    }
    BaseType_t rotation_task_result = xTaskCreateWithCaps(
        MusicRotationTask, "music_disc_rotate", 6144, this, 1, &music_rotation_task_,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (rotation_task_result != pdPASS) {
        music_rotation_task_ = nullptr;
        ESP_LOGE(TAG, "Failed to create music disc decode task");
    }
}

EmoteEngine::~EmoteEngine()
{
    if (music_fallback_timer_) {
        esp_timer_stop(music_fallback_timer_);
        esp_timer_delete(music_fallback_timer_);
        music_fallback_timer_ = nullptr;
    }
    if (music_release_timer_) {
        esp_timer_stop(music_release_timer_);
        esp_timer_delete(music_release_timer_);
        music_release_timer_ = nullptr;
    }
    if (music_rotation_timer_) {
        esp_timer_stop(music_rotation_timer_);
        esp_timer_delete(music_rotation_timer_);
        music_rotation_timer_ = nullptr;
    }
    music_rotation_task_stopping_ = true;
    if (music_rotation_task_) {
        xTaskNotifyGive(music_rotation_task_);
        for (int attempt = 0; attempt < 50 && music_rotation_task_; ++attempt) {
            vTaskDelay(pdMS_TO_TICKS(2));
        }
        if (music_rotation_task_) {
            vTaskDeleteWithCaps(music_rotation_task_);
            music_rotation_task_ = nullptr;
        }
    }
    if (engine_handle_) {
        Lock();
        ClearMusicArtworkLocked();
        Unlock();
    }
    if (engine_handle_) {
        gfx_emote_deinit(engine_handle_);
        engine_handle_ = nullptr;
    }

    if (font_tips) {
        gfx_label_delete_font(font_tips);
        font_tips = nullptr;
    }

    if (font_time) {
        gfx_label_delete_font(font_time);
        font_time = nullptr;
    }

    if (assets_handle_) {
        mmap_assets_del(assets_handle_);
        assets_handle_ = nullptr;
    }
}

void EmoteEngine::ClearMusicArtworkLocked()
{
    gfx_img_set_src(obj_img_music_background, nullptr);
    gfx_img_set_src(obj_img_music_disc, nullptr);
    if (music_background_data_) {
        heap_caps_free(music_background_data_);
        music_background_data_ = nullptr;
    }
    if (music_disc_source_) {
        heap_caps_free(music_disc_source_);
        music_disc_source_ = nullptr;
    }
    if (music_disc_frame_) {
        heap_caps_free(music_disc_frame_);
        music_disc_frame_ = nullptr;
    }
    if (music_disc_back_frame_) {
        heap_caps_free(music_disc_back_frame_);
        music_disc_back_frame_ = nullptr;
    }
    music_background_dsc_ = {};
    music_disc_dsc_ = {};
}

void EmoteEngine::InitializeMusicDiscBuffer(uint8_t* buffer)
{
    if (!buffer) {
        return;
    }
    constexpr int size = 192;
    constexpr size_t pixels = size * size;
    auto* alpha = buffer + pixels * 2;
    memset(alpha, 0, pixels);
    constexpr float center = 95.5f;
    for (int y = 0; y < size; ++y) {
        for (int x = music_disc_left_[y]; x <= music_disc_right_[y]; ++x) {
            const float dx = static_cast<float>(x) - center;
            const float dy = static_cast<float>(y) - center;
            const float radius = sqrtf(dx * dx + dy * dy);
            alpha[y * size + x] = radius <= 92.0f
                ? 0xFF
                : static_cast<uint8_t>(std::max(0.0f, (95.0f - radius) * 85.0f));
        }
    }
}

void EmoteEngine::WaitForMusicRotationIdle()
{
    while (music_rotation_busy_.load(std::memory_order_acquire)) {
        vTaskDelay(1);
    }
}

void EmoteEngine::CreateFallbackDiscLocked()
{
    constexpr int size = 192;
    constexpr size_t pixels = size * size;
    music_disc_source_ = static_cast<uint8_t*>(heap_caps_malloc(pixels * 2,
                                                               MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    music_disc_frame_ = static_cast<uint8_t*>(heap_caps_malloc(pixels * 3,
                                                              MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    music_disc_back_frame_ = static_cast<uint8_t*>(heap_caps_malloc(pixels * 3,
                                                                   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!music_disc_source_ || !music_disc_frame_ || !music_disc_back_frame_) {
        heap_caps_free(music_disc_source_);
        heap_caps_free(music_disc_frame_);
        heap_caps_free(music_disc_back_frame_);
        music_disc_source_ = nullptr;
        music_disc_frame_ = nullptr;
        music_disc_back_frame_ = nullptr;
        return;
    }
    auto* source = reinterpret_cast<uint16_t*>(music_disc_source_);
    auto* frame = reinterpret_cast<uint16_t*>(music_disc_frame_);
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            const int dx = x - size / 2;
            const int dy = y - size / 2;
            const int radius2 = dx * dx + dy * dy;
            const size_t index = y * size + x;
            const bool label = radius2 <= 28 * 28;
            source[index] = label ? 0x4A9F : ((radius2 / 90) % 2 ? 0x18E3 : 0x2104);
            frame[index] = source[index];
        }
    }
    memcpy(music_disc_back_frame_, music_disc_frame_, pixels * 2);
    InitializeMusicDiscBuffer(music_disc_frame_);
    InitializeMusicDiscBuffer(music_disc_back_frame_);
    music_disc_dsc_.header.magic = C_ARRAY_HEADER_MAGIC;
    music_disc_dsc_.header.cf = GFX_COLOR_FORMAT_RGB565A8;
    music_disc_dsc_.header.w = size;
    music_disc_dsc_.header.h = size;
    music_disc_dsc_.header.stride = size * 2;
    music_disc_dsc_.data_size = pixels * 3;
    music_disc_dsc_.data = music_disc_frame_;
    gfx_img_set_src(obj_img_music_disc, &music_disc_dsc_);
}

void EmoteEngine::CommitMusicSceneLocked()
{
    const bool show_music = music_overlay_requested_.load();
    music_overlay_visible_ = show_music;
    if (!show_music) {
        return;
    }
    gfx_emote_set_bg_color(engine_handle_, GFX_COLOR_HEX(0x08101E));
    SetMusicObjectsVisible(true);
    // Fallback deliberately has no full-screen bitmap; the solid background,
    // disc and labels are nevertheless committed as one complete theme.
    gfx_obj_set_visible(obj_img_music_background, music_background_data_ != nullptr);
    gfx_obj_set_visible(obj_anim_eye, false);
    gfx_obj_set_visible(obj_anim_mic, false);
    gfx_obj_set_visible(obj_img_icon, false);
    gfx_obj_set_visible(obj_label_tips, false);
    gfx_obj_set_visible(obj_label_time, false);
    gfx_anim_stop(obj_anim_eye);
    gfx_anim_set_segment(obj_anim_eye, 0, 0xFFFF, 5, false);
}

void EmoteEngine::EnterMusicScene(const MusicTrackInfo& track)
{
    if (!engine_handle_) {
        return;
    }
    const bool first_entry = !music_scene_active_.load();
    const bool request_music = first_entry || music_overlay_requested_.load();
    if (first_entry) {
        if (music_release_timer_) {
            esp_timer_stop(music_release_timer_);
        }
        music_scene_internal_free_before_ = heap_caps_get_free_size(
            MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        music_scene_spiram_free_before_ = heap_caps_get_free_size(
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        ESP_LOGI(TAG, "MUSIC_METRIC scene enter internal_free=%u psram_free=%u",
                 static_cast<unsigned>(music_scene_internal_free_before_),
                 static_cast<unsigned>(music_scene_spiram_free_before_));
    }
    music_rotation_paused_ = true;
    if (music_rotation_timer_) {
        esp_timer_stop(music_rotation_timer_);
    }
    WaitForMusicRotationIdle();
    Lock();
    music_scene_active_ = true;
    music_overlay_requested_ = request_music;
    music_overlay_visible_ = false;
    music_artwork_ready_ = false;
    music_scene_started_us_ = esp_timer_get_time();
    music_disc_angle_ = 0;
    ClearMusicArtworkLocked();
    gfx_label_set_text(obj_label_music_title, track.title.c_str());
    gfx_label_set_text(obj_label_music_artist, track.artist.empty() ? "正在播放" : track.artist.c_str());
    gfx_label_set_text(obj_label_music_previous, "");
    gfx_label_set_text(obj_label_music_current, "歌词加载中…");
    gfx_label_set_text(obj_label_music_next, "");
    gfx_label_set_text(obj_label_music_progress, "--:--");
    // Preparing a track must not expose a half-built scene. Keep every music
    // object hidden and leave the current eye frame untouched until both the
    // full-screen background and disc have been installed.
    SetMusicObjectsVisible(false);
    Unlock();
    music_rotation_paused_ = true;
    if (music_fallback_timer_) {
        esp_timer_stop(music_fallback_timer_);
        esp_timer_start_once(music_fallback_timer_, 3 * 1000 * 1000);
    }
}

void EmoteEngine::UpdateMusicTrackInfo(const MusicTrackInfo& track)
{
    if (!music_scene_active_) {
        return;
    }
    Lock();
    gfx_label_set_text(obj_label_music_title, track.title.c_str());
    gfx_label_set_text(obj_label_music_artist,
                       track.artist.empty() ? "正在播放" : track.artist.c_str());
    Unlock();
}

void EmoteEngine::SetMusicArtwork(const uint16_t* background, int background_width,
                                  int background_height, const uint16_t* disc,
                                  int disc_width, int disc_height)
{
    if (!music_scene_active_ || !background || !disc || background_width != 360 ||
        background_height != 360 || disc_width != 192 || disc_height != 192) {
        return;
    }
    const size_t background_pixels = background_width * background_height;
    const size_t disc_pixels = disc_width * disc_height;
    auto* new_background = static_cast<uint8_t*>(heap_caps_malloc(background_pixels * 3,
                                                                  MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    auto* new_disc_source = static_cast<uint8_t*>(heap_caps_malloc(disc_pixels * 2,
                                                                   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    auto* new_disc_frame = static_cast<uint8_t*>(heap_caps_malloc(disc_pixels * 3,
                                                                  MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    auto* new_disc_back_frame = static_cast<uint8_t*>(heap_caps_malloc(
        disc_pixels * 3, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!new_background || !new_disc_source || !new_disc_frame || !new_disc_back_frame) {
        heap_caps_free(new_background);
        heap_caps_free(new_disc_source);
        heap_caps_free(new_disc_frame);
        heap_caps_free(new_disc_back_frame);
        return;
    }
    memcpy(new_background, background, background_pixels * 2);
    memset(new_background + background_pixels * 2, 0xFF, background_pixels);
    memcpy(new_disc_source, disc, disc_pixels * 2);
    memcpy(new_disc_frame, disc, disc_pixels * 2);
    memcpy(new_disc_back_frame, disc, disc_pixels * 2);
    InitializeMusicDiscBuffer(new_disc_frame);
    InitializeMusicDiscBuffer(new_disc_back_frame);

    music_rotation_paused_ = true;
    if (music_rotation_timer_) {
        esp_timer_stop(music_rotation_timer_);
    }
    WaitForMusicRotationIdle();
    Lock();
    if (!music_scene_active_) {
        Unlock();
        heap_caps_free(new_background);
        heap_caps_free(new_disc_source);
        heap_caps_free(new_disc_frame);
        heap_caps_free(new_disc_back_frame);
        return;
    }
    ClearMusicArtworkLocked();
    music_background_data_ = new_background;
    music_disc_source_ = new_disc_source;
    music_disc_frame_ = new_disc_frame;
    music_disc_back_frame_ = new_disc_back_frame;
    music_background_dsc_.header.magic = C_ARRAY_HEADER_MAGIC;
    music_background_dsc_.header.cf = GFX_COLOR_FORMAT_RGB565A8;
    music_background_dsc_.header.w = background_width;
    music_background_dsc_.header.h = background_height;
    music_background_dsc_.header.stride = background_width * 2;
    music_background_dsc_.data_size = background_pixels * 3;
    music_background_dsc_.data = music_background_data_;
    music_disc_dsc_.header.magic = C_ARRAY_HEADER_MAGIC;
    music_disc_dsc_.header.cf = GFX_COLOR_FORMAT_RGB565A8;
    music_disc_dsc_.header.w = disc_width;
    music_disc_dsc_.header.h = disc_height;
    music_disc_dsc_.header.stride = disc_width * 2;
    music_disc_dsc_.data_size = disc_pixels * 3;
    music_disc_dsc_.data = music_disc_frame_;
    gfx_img_set_src(obj_img_music_background, &music_background_dsc_);
    gfx_img_set_src(obj_img_music_disc, &music_disc_dsc_);
    music_artwork_ready_ = true;
    const bool show_music = music_overlay_requested_.load();
    CommitMusicSceneLocked();
    if (show_music) {
        ESP_LOGI(TAG, "MUSIC_METRIC artwork commit overlay=1");
    }
    Unlock();
    if (music_fallback_timer_) {
        esp_timer_stop(music_fallback_timer_);
    }
    music_rotation_paused_ = !show_music;
    if (show_music && music_rotation_timer_ && music_rotation_task_) {
        esp_timer_start_periodic(music_rotation_timer_, 100 * 1000);
    }
}

void EmoteEngine::CommitMusicFallback()
{
    if (!music_scene_active_ || music_artwork_ready_) {
        return;
    }
    music_rotation_paused_ = true;
    WaitForMusicRotationIdle();
    Lock();
    if (!music_scene_active_ || music_artwork_ready_) {
        Unlock();
        return;
    }
    ClearMusicArtworkLocked();
    CreateFallbackDiscLocked();
    if (!music_disc_source_) {
        Unlock();
        ESP_LOGE(TAG, "Failed to allocate fallback music scene");
        return;
    }
    music_artwork_ready_ = true;
    const bool show_music = music_overlay_requested_.load();
    CommitMusicSceneLocked();
    Unlock();
    if (music_fallback_timer_) {
        esp_timer_stop(music_fallback_timer_);
    }
    music_rotation_paused_ = !show_music;
    if (show_music && music_rotation_timer_ && music_rotation_task_) {
        esp_timer_start_periodic(music_rotation_timer_, 100 * 1000);
    }
    ESP_LOGI(TAG, "MUSIC_METRIC fallback commit overlay=%d elapsed_ms=%d", show_music,
             static_cast<int>((esp_timer_get_time() - music_scene_started_us_.load()) / 1000));
}

void EmoteEngine::SetMusicLyrics(const std::string& previous, const std::string& current,
                                 const std::string& next)
{
    if (!music_scene_active_) {
        return;
    }
    Lock();
    gfx_label_set_text(obj_label_music_previous, previous.c_str());
    gfx_label_set_text(obj_label_music_current, current.c_str());
    gfx_label_set_text(obj_label_music_next, next.c_str());
    Unlock();
}

void EmoteEngine::SetMusicProgress(int position_ms, int duration_ms)
{
    if (!music_scene_active_) {
        return;
    }
    char text[24];
    const int position_seconds = std::max(0, position_ms / 1000);
    if (duration_ms > 0) {
        const int duration_seconds = duration_ms / 1000;
        snprintf(text, sizeof(text), "%02d:%02d / %02d:%02d",
                 position_seconds / 60, position_seconds % 60,
                 duration_seconds / 60, duration_seconds % 60);
    } else {
        snprintf(text, sizeof(text), "%02d:%02d", position_seconds / 60,
                 position_seconds % 60);
    }
    Lock();
    gfx_label_set_text(obj_label_music_progress, text);
    Unlock();
}

void EmoteEngine::SetMusicOverlayVisible(bool visible)
{
    if (!music_scene_active_) {
        return;
    }
    music_overlay_requested_ = visible;
    const bool actual_visible = visible && music_artwork_ready_.load();
    const bool visibility_changed =
        music_overlay_visible_.exchange(actual_visible) != actual_visible;
    if (visibility_changed) {
        ESP_LOGI(TAG, "MUSIC_METRIC overlay visible=%d", actual_visible);
    }
    music_rotation_paused_ = !actual_visible;
    if (!actual_visible && visibility_changed && music_rotation_timer_) {
        esp_timer_stop(music_rotation_timer_);
    }
    // Before artwork is ready, retain the current expression exactly as-is.
    // ApplyRenderModel() will keep normal eyes current while this function
    // only records the requested music ownership.
    if (visible && !actual_visible) {
        return;
    }
    Lock();
    gfx_emote_set_bg_color(engine_handle_, GFX_COLOR_HEX(actual_visible ? 0x08101E : 0x000000));
    SetMusicObjectsVisible(actual_visible);
    if (actual_visible) {
        gfx_obj_set_visible(obj_img_music_background, music_background_data_ != nullptr);
    }
    gfx_obj_set_visible(obj_anim_eye, !actual_visible);
    gfx_obj_set_visible(obj_anim_mic, false);
    gfx_obj_set_visible(obj_img_icon, !actual_visible);
    gfx_obj_set_visible(obj_label_tips, !actual_visible);
    gfx_obj_set_visible(obj_label_time, false);
    if (actual_visible) {
        // ExpressionDirector may have selected a 20 FPS media expression just
        // before restoring the music layer. Keep the hidden animation stopped
        // and return the shared graphics cadence to the safe music rate.
        gfx_anim_stop(obj_anim_eye);
        gfx_anim_set_segment(obj_anim_eye, 0, 0xFFFF, 5, false);
    } else {
        // ApplyRenderModel() immediately selects and starts the requested eye
        // animation after hiding the music scene. Mark it dirty here so the
        // first interaction frame replaces the full-screen artwork at once.
        obj_anim_eye->is_dirty = true;
    }
    Unlock();
    if (actual_visible && visibility_changed && music_rotation_timer_ && music_rotation_task_) {
        const esp_err_t result = esp_timer_start_periodic(music_rotation_timer_, 100 * 1000);
        if (result != ESP_OK && result != ESP_ERR_INVALID_STATE) {
            ESP_LOGW(TAG, "Failed to resume music rotation timer: %s",
                     esp_err_to_name(result));
        }
    }
}

void EmoteEngine::ExitMusicScene()
{
    if (!music_scene_active_.exchange(false)) {
        return;
    }
    if (music_fallback_timer_) {
        esp_timer_stop(music_fallback_timer_);
    }
    music_rotation_paused_ = true;
    if (music_rotation_timer_) {
        esp_timer_stop(music_rotation_timer_);
    }
    Lock();
    music_overlay_requested_ = false;
    music_overlay_visible_ = false;
    music_artwork_ready_ = false;
    Unlock();
    WaitForMusicRotationIdle();
    Lock();
    SetMusicObjectsVisible(false);
    ClearMusicArtworkLocked();
    gfx_emote_set_bg_color(engine_handle_, GFX_COLOR_HEX(0x000000));
    gfx_obj_set_visible(obj_anim_eye, true);
    gfx_obj_set_visible(obj_img_icon, false);
    // Restore exactly one normal overlay before ExpressionDirector applies
    // the current state. This prevents the periodic clock callback and stale
    // music labels from becoming visible together during the hand-off.
    SetUIDisplayMode(UIDisplayMode::SHOW_NONE);
    gfx_anim_start(obj_anim_eye);
    obj_anim_eye->is_dirty = true;  // animation dirtiness forces a full refresh
    Unlock();

    ESP_LOGI(TAG, "MUSIC_METRIC scene exit requested");
    if (music_release_timer_) {
        esp_timer_stop(music_release_timer_);
        esp_timer_start_once(music_release_timer_, 5 * 1000 * 1000);
    }
}

void EmoteEngine::MusicRotationTimer(void* arg)
{
    auto* engine = static_cast<EmoteEngine*>(arg);
    if (engine && engine->music_rotation_task_) {
        xTaskNotifyGive(engine->music_rotation_task_);
    }
}

void EmoteEngine::MusicFallbackTimer(void* arg)
{
    auto* engine = static_cast<EmoteEngine*>(arg);
    if (!engine || !engine->music_scene_active_ || engine->music_artwork_ready_ ||
        esp_timer_get_time() - engine->music_scene_started_us_.load() < 3 * 1000 * 1000) {
        return;
    }
    Application::GetInstance().Schedule([engine]() { engine->CommitMusicFallback(); });
}

void EmoteEngine::MusicReleaseTimer(void* arg)
{
    auto* engine = static_cast<EmoteEngine*>(arg);
    if (!engine || engine->music_scene_active_) {
        return;
    }
    const size_t internal_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    const size_t spiram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    ESP_LOGI(TAG,
             "MUSIC_METRIC resources released elapsed_ms=5000 internal_free=%u internal_delta=%d psram_free=%u psram_delta=%d",
             static_cast<unsigned>(internal_free),
             static_cast<int>(internal_free) - static_cast<int>(engine->music_scene_internal_free_before_),
             static_cast<unsigned>(spiram_free),
             static_cast<int>(spiram_free) - static_cast<int>(engine->music_scene_spiram_free_before_));
}

void EmoteEngine::MusicRotationTask(void* arg)
{
    auto* engine = static_cast<EmoteEngine*>(arg);
    while (!engine->music_rotation_task_stopping_) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (!engine->music_rotation_task_stopping_) {
            engine->RotateMusicDisc();
            // Yield after each native-frame copy so audio remains dominant.
            vTaskDelay(1);
        }
    }
    engine->music_rotation_task_ = nullptr;
    vTaskDeleteWithCaps(nullptr);
}

void EmoteEngine::RotateMusicDisc()
{
    if (!music_scene_active_ || music_rotation_paused_) {
        return;
    }
    constexpr int size = 192;
    constexpr int32_t center_fp = 191 << 14;  // 95.5 in Q15
    const int64_t started_us = esp_timer_get_time();

    Lock();
    if (!music_scene_active_ || music_rotation_paused_ || music_rotation_busy_ ||
        !music_disc_source_ || !music_disc_frame_ || !music_disc_back_frame_) {
        Unlock();
        return;
    }
    music_rotation_busy_.store(true, std::memory_order_release);
    auto* source_bytes = music_disc_source_;
    auto* output_bytes = music_disc_back_frame_;
    music_disc_angle_ += 1.2f;
    if (music_disc_angle_ >= 360.0f) {
        music_disc_angle_ -= 360.0f;
    }
    const float angle = music_disc_angle_;
    Unlock();

    const float radians = -angle * 3.14159265f / 180.0f;
    const int32_t cosine = static_cast<int32_t>(cosf(radians) * 32768);
    const int32_t sine = static_cast<int32_t>(sinf(radians) * 32768);
    auto* source = reinterpret_cast<const uint16_t*>(source_bytes);
    auto* output = reinterpret_cast<uint16_t*>(output_bytes);
    auto byte_swap = [](uint16_t value) -> uint16_t {
        return static_cast<uint16_t>((value << 8) | (value >> 8));
    };
    auto lerp = [](int first, int second, int fraction) -> int {
        return first + (((second - first) * fraction + 128) >> 8);
    };

    for (int y = 0; y < size; ++y) {
        const int first_x = music_disc_left_[y];
        const int last_x = music_disc_right_[y];
        const int32_t dx_fp = (first_x << 15) - center_fp;
        const int32_t dy_fp = (y << 15) - center_fp;
        int32_t source_x_fp = center_fp + static_cast<int32_t>(
            (static_cast<int64_t>(cosine) * dx_fp - static_cast<int64_t>(sine) * dy_fp) >> 15);
        int32_t source_y_fp = center_fp + static_cast<int32_t>(
            (static_cast<int64_t>(sine) * dx_fp + static_cast<int64_t>(cosine) * dy_fp) >> 15);
        for (int x = first_x; x <= last_x; ++x) {
            const size_t index = y * size + x;
            const int source_x = source_x_fp >> 15;
            const int source_y = source_y_fp >> 15;
            if (source_x >= 0 && source_x + 1 < size &&
                source_y >= 0 && source_y + 1 < size) {
                const int fx = (source_x_fp & 0x7FFF) >> 7;
                const int fy = (source_y_fp & 0x7FFF) >> 7;
                const uint16_t p00 = byte_swap(source[source_y * size + source_x]);
                const uint16_t p10 = byte_swap(source[source_y * size + source_x + 1]);
                const uint16_t p01 = byte_swap(source[(source_y + 1) * size + source_x]);
                const uint16_t p11 = byte_swap(source[(source_y + 1) * size + source_x + 1]);
                const int red = lerp(
                    lerp((p00 >> 11) & 0x1F, (p10 >> 11) & 0x1F, fx),
                    lerp((p01 >> 11) & 0x1F, (p11 >> 11) & 0x1F, fx), fy);
                const int green = lerp(
                    lerp((p00 >> 5) & 0x3F, (p10 >> 5) & 0x3F, fx),
                    lerp((p01 >> 5) & 0x3F, (p11 >> 5) & 0x3F, fx), fy);
                const int blue = lerp(
                    lerp(p00 & 0x1F, p10 & 0x1F, fx),
                    lerp(p01 & 0x1F, p11 & 0x1F, fx), fy);
                output[index] = byte_swap(
                    static_cast<uint16_t>((red << 11) | (green << 5) | blue));
            }
            source_x_fp += cosine;
            source_y_fp += sine;
        }
    }

    Lock();
    if (music_scene_active_ && !music_rotation_paused_ &&
        music_disc_source_ == source_bytes && music_disc_back_frame_ == output_bytes) {
        std::swap(music_disc_frame_, music_disc_back_frame_);
        music_disc_dsc_.data = music_disc_frame_;
        obj_img_music_disc->is_dirty = true;
    }
    music_rotation_busy_.store(false, std::memory_order_release);
    Unlock();

    const int64_t elapsed_us = esp_timer_get_time() - started_us;
    static uint32_t slow_frame_count = 0;
    if (elapsed_us > 250000 && (++slow_frame_count % 20) == 1) {
        ESP_LOGW(TAG, "Music disc rotation remains back-pressured (%d ms); frames are being dropped",
                 static_cast<int>(elapsed_us / 1000));
    }
}

void EmoteEngine::setEyes(int aaf, bool repeat, int fps)
{
    if (!engine_handle_ || !assets_handle_) {
        return;
    }

    const void* src_data = mmap_assets_get_mem(assets_handle_, aaf);
    size_t src_len = mmap_assets_get_size(assets_handle_, aaf);
    if (src_data == nullptr || src_len == 0) {
        ESP_LOGE(TAG, "Cannot load expression asset %d; falling back to neutral", aaf);
        src_data = mmap_assets_get_mem(assets_handle_, MMAP_EMOJI_NORMAL_NEUTRAL_EAF);
        src_len = mmap_assets_get_size(assets_handle_, MMAP_EMOJI_NORMAL_NEUTRAL_EAF);
        if (src_data == nullptr || src_len == 0) {
            ESP_LOGE(TAG, "Neutral expression asset is unavailable; keeping current frame");
            return;
        }
        repeat = true;
        fps = 20;
    }

    Lock();
    gfx_anim_set_src(obj_anim_eye, src_data, src_len);
    gfx_anim_set_segment(obj_anim_eye, 0, 0xFFFF, fps, repeat);
    gfx_anim_start(obj_anim_eye);
    Unlock();
}

void EmoteEngine::stopEyes()
{
    // Implementation if needed
}

void EmoteEngine::Lock()
{
    if (engine_handle_) {
        gfx_emote_lock(engine_handle_);
    }
}

void EmoteEngine::Unlock()
{
    if (engine_handle_) {
        gfx_emote_unlock(engine_handle_);
    }
}

void EmoteEngine::SetIcon(int asset_id)
{
    if (!engine_handle_) {
        return;
    }

    Lock();
    if (!SetupImageDescriptor(assets_handle_, &icon_img_dsc, asset_id)) {
        return;
    }
    gfx_img_set_src(obj_img_icon, static_cast<void*>(&icon_img_dsc));
    current_icon_type = asset_id;
    Unlock();
}

bool EmoteEngine::OnFlushIoReady(esp_lcd_panel_io_handle_t panel_io,
                                 esp_lcd_panel_io_event_data_t* edata,
                                 void* user_ctx)
{
    return true;
}

void EmoteEngine::OnFlush(gfx_handle_t handle, int x_start, int y_start,
                          int x_end, int y_end, const void* color_data)
{
    auto* panel = static_cast<esp_lcd_panel_handle_t>(gfx_emote_get_user_data(handle));
    if (panel) {
        esp_lcd_panel_draw_bitmap(panel, x_start, y_start, x_end, y_end, color_data);
    }
    gfx_emote_flush_ready(handle, true);
}

// EmoteDisplay implementation
EmoteDisplay::EmoteDisplay(esp_lcd_panel_handle_t panel, esp_lcd_panel_io_handle_t panel_io)
{
    InitializeEngine(panel, panel_io);
    InitializeDirector();
}

EmoteDisplay::~EmoteDisplay() = default;

void EmoteDisplay::SetBehavior(const DisplayBehaviorRequest& request)
{
    if (!director_) {
        return;
    }

#if CONFIG_ECHOEAR_MUSIC_SCENE
    if (request.source == DisplayBehaviorSource::kMusic &&
        (request.behavior == DisplayBehavior::kMusicBuffering ||
         request.behavior == DisplayBehavior::kMusicPlaying ||
         request.behavior == DisplayBehavior::kMusicPaused)) {
        // StartStreaming posts this authoritative state immediately after the
        // scene is entered. From this point onward genuine higher-priority
        // interaction states are allowed to cover the music UI.
        music_scene_behavior_ready_ = true;
    }
#endif

    if (request.source == DisplayBehaviorSource::kDeviceState) {
        director_->SetBaseBehavior(request);
    } else if (request.source == DisplayBehaviorSource::kMusic) {
        if (request.behavior == DisplayBehavior::kIdle) {
            director_->ClearMediaBehavior();
        } else if (request.behavior == DisplayBehavior::kMusicBuffering ||
                   request.behavior == DisplayBehavior::kMusicPlaying ||
                   request.behavior == DisplayBehavior::kMusicPaused) {
            director_->SetMediaBehavior(request);
        } else {
            director_->PostTransientBehavior(request);
        }
    } else {
        director_->PostTransientBehavior(request);
    }

}

void EmoteDisplay::SetEmotion(const char* emotion)
{
    if (!engine_) {
        return;
    }

    if (director_) {
#if CONFIG_ECHOEAR_CLOUD_EMOTION
        director_->SetCloudEmotion(emotion);
#endif
        return;
    }

    using EmotionParam = std::tuple<int, bool, int>;
    static const std::unordered_map<std::string, EmotionParam> emotion_map = {
        {"happy",       {MMAP_EMOJI_NORMAL_HAPPY_EAF,         true,  20}},
        {"laughing",    {MMAP_EMOJI_NORMAL_HAPPY_EAF,         true,  20}},
        {"funny",       {MMAP_EMOJI_NORMAL_HAPPY_EAF,         true,  20}},
        {"loving",      {MMAP_EMOJI_NORMAL_HAPPY_EAF,         true,  20}},
        {"embarrassed", {MMAP_EMOJI_NORMAL_CONFUSED_EAF,      true,  20}},
        {"confident",   {MMAP_EMOJI_NORMAL_HAPPY_EAF,         true,  20}},
        {"delicious",   {MMAP_EMOJI_NORMAL_HAPPY_EAF,         true,  20}},
        {"sad",         {MMAP_EMOJI_NORMAL_SAD_EAF,           true,  20}},
        {"crying",      {MMAP_EMOJI_NORMAL_CRY_EAF,           true,  20}},
        {"sleepy",      {MMAP_EMOJI_NORMAL_SLEEP_EAF,         true,  16}},
        {"silly",       {MMAP_EMOJI_NORMAL_CONFUSED_EAF,      true,  20}},
        {"angry",       {MMAP_EMOJI_NORMAL_ANGRY_EAF,         true,  20}},
        {"surprised",   {MMAP_EMOJI_NORMAL_SHOCKED_EAF,       true,  20}},
        {"shocked",     {MMAP_EMOJI_NORMAL_SHOCKED_EAF,       true,  20}},
        {"thinking",    {MMAP_EMOJI_NORMAL_CONFUSED_EAF,      true,  20}},
        {"winking",     {MMAP_EMOJI_NORMAL_WINKING_EAF,       false, 20}},
        {"relaxed",     {MMAP_EMOJI_NORMAL_NEUTRAL_EAF,       true,  20}},
        {"confused",    {MMAP_EMOJI_NORMAL_CONFUSED_EAF,      true,  20}},
        {"neutral",     {MMAP_EMOJI_NORMAL_NEUTRAL_EAF,       true,  20}},
        {"idle",        {MMAP_EMOJI_NORMAL_NEUTRAL_EAF,       true,  20}},
    };

    auto it = emotion_map.find(emotion);
    if (it != emotion_map.end()) {
        int aaf = std::get<0>(it->second);
        bool repeat = std::get<1>(it->second);
        int fps = std::get<2>(it->second);
        engine_->setEyes(aaf, repeat, fps);
    }
}

void EmoteDisplay::SetChatMessage(const char* role, const char* content)
{
    if (expression_test_running_) {
        return;
    }
    if (engine_ && engine_->IsMusicSceneActive() && engine_->IsMusicOverlayVisible()) {
        return;
    }
    engine_->Lock();
    if (content && strlen(content) > 0) {
        gfx_label_set_text(obj_label_tips, content);
        SetUIDisplayMode(UIDisplayMode::SHOW_TIPS);
    }
    engine_->Unlock();
}

void EmoteDisplay::EnterMusicScene(const MusicTrackInfo& track)
{
#if CONFIG_ECHOEAR_MUSIC_SCENE
    if (engine_) {
        if (!engine_->IsMusicSceneActive()) {
            music_scene_behavior_ready_ = false;
        }
        engine_->EnterMusicScene(track);
    }
#else
    Display::EnterMusicScene(track);
#endif
}

void EmoteDisplay::UpdateMusicTrackInfo(const MusicTrackInfo& track)
{
#if CONFIG_ECHOEAR_MUSIC_SCENE
    if (engine_) {
        engine_->UpdateMusicTrackInfo(track);
    }
#else
    Display::UpdateMusicTrackInfo(track);
#endif
}

void EmoteDisplay::SetMusicArtwork(const uint16_t* background, int background_width,
                                   int background_height, const uint16_t* disc,
                                   int disc_width, int disc_height)
{
#if CONFIG_ECHOEAR_MUSIC_SCENE
    if (engine_) {
        engine_->SetMusicArtwork(background, background_width, background_height,
                                 disc, disc_width, disc_height);
    }
#endif
}

void EmoteDisplay::CommitMusicFallback()
{
#if CONFIG_ECHOEAR_MUSIC_SCENE
    if (engine_) {
        engine_->CommitMusicFallback();
    }
#else
    Display::CommitMusicFallback();
#endif
}

void EmoteDisplay::SetMusicLyricWindow(const std::string& previous,
                                       const std::string& current,
                                       const std::string& next)
{
#if CONFIG_ECHOEAR_MUSIC_SCENE
    if (engine_) {
        engine_->SetMusicLyrics(previous, current, next);
    }
#else
    Display::SetMusicLyricWindow(previous, current, next);
#endif
}

void EmoteDisplay::UpdateMusicProgress(int position_ms, int duration_ms)
{
#if CONFIG_ECHOEAR_MUSIC_SCENE
    if (engine_) {
        engine_->SetMusicProgress(position_ms, duration_ms);
    }
#endif
}

void EmoteDisplay::ExitMusicScene()
{
#if CONFIG_ECHOEAR_MUSIC_SCENE
    music_scene_behavior_ready_ = false;
    if (engine_) {
        engine_->ExitMusicScene();
    }
    if (director_) {
        director_->ForceRender();
    }
#else
    Display::ExitMusicScene();
#endif
}

void EmoteDisplay::SetStatus(const char* status)
{
    if (!engine_) {
        return;
    }
    if (expression_test_running_) {
        return;
    }

    if (!director_ && std::strcmp(status, "聆听中...") == 0) {
        SetUIDisplayMode(UIDisplayMode::SHOW_ANIM_TOP);
        engine_->setEyes(MMAP_EMOJI_NORMAL_HAPPY_EAF, true, 20);
        engine_->SetIcon(MMAP_EMOJI_NORMAL_ICON_MIC_BIN);
    } else if (!director_ && std::strcmp(status, "待命") == 0) {
        SetUIDisplayMode(UIDisplayMode::SHOW_NONE);
    } else if (!director_ && std::strcmp(status, "说话中...") == 0) {
        SetUIDisplayMode(UIDisplayMode::SHOW_TIPS);
        engine_->SetIcon(MMAP_EMOJI_NORMAL_ICON_SPEAKER_ZZZ_BIN);
    } else if (!director_ && std::strcmp(status, "错误") == 0) {
        SetUIDisplayMode(UIDisplayMode::SHOW_TIPS);
        engine_->SetIcon(MMAP_EMOJI_NORMAL_ICON_WIFI_FAILED_BIN);
    }

    engine_->Lock();
    if (director_ || std::strcmp(status, "连接中...") != 0) {
        gfx_label_set_text(obj_label_tips, status);
    }
    engine_->Unlock();
}

void EmoteDisplay::InitializeEngine(esp_lcd_panel_handle_t panel, esp_lcd_panel_io_handle_t panel_io)
{
    engine_ = std::make_unique<EmoteEngine>(panel, panel_io);
}

void EmoteDisplay::InitializeDirector()
{
#if CONFIG_ECHOEAR_EXPRESSION_DIRECTOR
    director_ = std::make_unique<ExpressionDirector>(
        [this](const ExpressionRenderModel& render_model) {
            if (!expression_test_running_) {
                ApplyRenderModel(render_model);
            }
        });
#endif
}

bool EmoteDisplay::StartExpressionTest()
{
    bool expected = false;
    if (!expression_test_running_.compare_exchange_strong(expected, true)) {
        return false;
    }

    BaseType_t result = xTaskCreatePinnedToCore(
        ExpressionTestTask,
        "expression_test",
        4 * 1024,
        this,
        4,
        nullptr,
        0);
    if (result != pdPASS) {
        expression_test_running_ = false;
        ESP_LOGE(TAG, "Failed to create expression self-test task");
        return false;
    }
    return true;
}

void EmoteDisplay::ExpressionTestTask(void* arg)
{
    static_cast<EmoteDisplay*>(arg)->RunExpressionTest();
    vTaskDelete(nullptr);
}

void EmoteDisplay::RunExpressionTest()
{
    struct TestFrame {
        const char* name;
        ExpressionRenderModel render_model;
    };

    static const TestFrame frames[] = {
        {"neutral", {MMAP_EMOJI_NORMAL_NEUTRAL_EAF, true, 20,
                     MMAP_EMOJI_NORMAL_ICON_BATTERY_BIN, ExpressionUiMode::kTips}},
        {"listen", {MMAP_EMOJI_NORMAL_NEUTRAL_EAF, true, 20,
                     MMAP_EMOJI_NORMAL_ICON_MIC_BIN, ExpressionUiMode::kListening}},
        {"winking", {MMAP_EMOJI_NORMAL_WINKING_EAF, true, 12,
                     MMAP_EMOJI_NORMAL_ICON_BATTERY_BIN, ExpressionUiMode::kTips}},
        {"confused", {MMAP_EMOJI_NORMAL_CONFUSED_EAF, true, 20,
                      MMAP_EMOJI_NORMAL_ICON_BATTERY_BIN, ExpressionUiMode::kTips}},
        {"Happy", {MMAP_EMOJI_NORMAL_HAPPY_EAF, true, 20,
                   MMAP_EMOJI_NORMAL_ICON_BATTERY_BIN, ExpressionUiMode::kTips}},
        {"cry", {MMAP_EMOJI_NORMAL_CRY_EAF, true, 20,
                 MMAP_EMOJI_NORMAL_ICON_BATTERY_BIN, ExpressionUiMode::kTips}},
        {"Sad", {MMAP_EMOJI_NORMAL_SAD_EAF, true, 20,
                  MMAP_EMOJI_NORMAL_ICON_BATTERY_BIN, ExpressionUiMode::kTips}},
        {"sleep", {MMAP_EMOJI_NORMAL_SLEEP_EAF, true, 16,
                   MMAP_EMOJI_NORMAL_ICON_BATTERY_BIN, ExpressionUiMode::kTips}},
        {"angry", {MMAP_EMOJI_NORMAL_ANGRY_EAF, true, 20,
                   MMAP_EMOJI_NORMAL_ICON_BATTERY_BIN, ExpressionUiMode::kTips}},
        {"shocked", {MMAP_EMOJI_NORMAL_SHOCKED_EAF, true, 20,
                     MMAP_EMOJI_NORMAL_ICON_BATTERY_BIN, ExpressionUiMode::kTips}},
    };

    ESP_LOGI(TAG, "Expression self-test started (%u frames)",
             static_cast<unsigned>(sizeof(frames) / sizeof(frames[0])));
    for (const auto& frame : frames) {
        Application::GetInstance().Schedule([this, frame]() {
            ESP_LOGI(TAG, "Expression self-test frame: %s asset=%d",
                     frame.name, frame.render_model.animation_asset_id);
            ApplyExpressionTestFrame(frame.name, frame.render_model);
        });
        vTaskDelay(pdMS_TO_TICKS(2500));
    }

    Application::GetInstance().Schedule([this]() {
        expression_test_running_ = false;
        ESP_LOGI(TAG, "Expression self-test finished; restoring live state");
        if (director_) {
            director_->ForceRender();
        }
    });
}

void EmoteDisplay::ApplyExpressionTestFrame(const char* name,
                                            const ExpressionRenderModel& render_model)
{
    ApplyRenderModel(render_model);
    engine_->Lock();
    gfx_label_set_text(obj_label_tips, name);
    if (render_model.ui_mode != ExpressionUiMode::kListening) {
        SetUIDisplayMode(UIDisplayMode::SHOW_TIPS);
    }
    engine_->Unlock();
}

void EmoteDisplay::ApplyRenderModel(const ExpressionRenderModel& render_model)
{
    if (!engine_) {
        return;
    }

#if CONFIG_ECHOEAR_MUSIC_SCENE
    if (engine_->IsMusicSceneActive()) {
        // Before the first music behavior arrives, callbacks can still belong
        // to the speaking/listening hand-off that initiated playback. Keep the
        // scene visible during that narrow window to avoid a one-frame flash.
        // Afterwards the director's selected semantic state is authoritative:
        // P0-P3 interactions cover music, and media state restores it.
        const bool show_music = !music_scene_behavior_ready_ ||
                                render_model.music_scene_visible;
        engine_->SetMusicOverlayVisible(show_music);
        if (engine_->IsMusicOverlayVisible()) {
            return;
        }
    }
#endif

    engine_->setEyes(render_model.animation_asset_id, render_model.repeat, render_model.fps);
    if (render_model.ui_mode != ExpressionUiMode::kImmersive) {
        engine_->SetIcon(render_model.icon_asset_id);
    }

    engine_->Lock();
    if (!render_model.text.empty()) {
        gfx_label_set_text(obj_label_tips, render_model.text.c_str());
    }
    switch (render_model.ui_mode) {
    case ExpressionUiMode::kImmersive:
        SetUIDisplayMode(UIDisplayMode::SHOW_NONE);
        break;
    case ExpressionUiMode::kTips:
        SetUIDisplayMode(UIDisplayMode::SHOW_TIPS);
        break;
    case ExpressionUiMode::kTime:
        SetUIDisplayMode(UIDisplayMode::SHOW_TIME);
        break;
    case ExpressionUiMode::kListening:
        SetUIDisplayMode(UIDisplayMode::SHOW_ANIM_TOP);
        break;
    }
    engine_->Unlock();
}

bool EmoteDisplay::Lock(int timeout_ms)
{
    return true;
}

void EmoteDisplay::Unlock()
{
    // Implementation if needed
}

} // namespace anim
