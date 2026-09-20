#pragma once

#include "character_action.h"
#include "display/emote_display.h"

#include <atomic>
#include <cstdint>
#include <string>

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <gfx.h>

namespace emote {

class VocatCatDisplay final : public EmoteDisplay {
public:
    VocatCatDisplay(esp_lcd_panel_handle_t panel, esp_lcd_panel_io_handle_t panel_io, int width,
                    int height);
    ~VocatCatDisplay() override;

    void LoadAssets() override;
    void SetStatus(const char* status) override;
    void SetEmotion(const char* emotion) override;
    void SetChatMessage(const char* role, const char* content) override;
    bool SupportsCharacterActions() const override { return true; }
    bool RequestCharacterAction(const std::string& name) override;
    bool SupportsMusicCompanionSettings() const override { return true; }
    bool ConfigureMusicCompanion(const std::string& mode, const std::string& instrument) override;
    void SetMusicPlaybackActive(bool active) override;
    void SetMusicTrackInfo(const MusicTrackInfo& track) override;
    void SetMusicArtwork(const uint16_t* background, int background_width, int background_height,
                         const uint16_t* disc, int disc_width, int disc_height) override;
    void SetMusicLyricWindow(const std::string& previous, const std::string& current,
                             const std::string& next) override;
    void UpdateMusicProgress(int position_ms, int duration_ms) override;

private:
    static void RenderTaskEntry(void* context);
    void RenderTask();
    void StopRenderer();
    void ClearSubtitle();
    void SetMusicUiVisible(bool visible);
    bool RenderRotatingDisc(uint8_t* output, double seconds);
    anim::CharacterPreview CurrentBaseScene() const;

    SemaphoreHandle_t state_mutex_ = nullptr;
    TaskHandle_t render_task_ = nullptr;
    gfx_obj_t* character_image_ = nullptr;
    uint8_t* frame_buffers_[2] = {nullptr, nullptr};
    uint8_t* cover_buffer_ = nullptr;
    uint8_t* disc_source_ = nullptr;
    uint8_t* disc_buffers_[2] = {nullptr, nullptr};
    int16_t disc_left_[192] = {};
    int16_t disc_right_[192] = {};
    gfx_image_dsc_t frame_descriptors_[2] = {};
    gfx_image_dsc_t cover_descriptor_ = {};
    gfx_image_dsc_t disc_descriptor_ = {};
    gfx_obj_t* music_disc_image_ = nullptr;
    gfx_obj_t* music_labels_[6] = {};
    void* music_font_ = nullptr;
    anim::CharacterActionRequest action_request_;
    std::atomic<bool> stopping_{false};
    std::atomic<bool> music_active_{false};
    std::atomic<bool> artwork_ready_{false};
    std::atomic<bool> idle_{false};
    std::atomic<int> status_scene_{static_cast<int>(anim::CharacterPreview::kStartup)};
    std::atomic<int> emotion_scene_{static_cast<int>(anim::CharacterPreview::kEyes)};
    std::atomic<int> companion_preference_{1};
    std::atomic<int64_t> music_started_us_{0};
};

}  // namespace emote
