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
    void SetMusicCoverArtwork(const uint16_t* pixels, int width, int height) override;

private:
    static void RenderTaskEntry(void* context);
    void RenderTask();
    void StopRenderer();
    void ClearSubtitle();
    anim::CharacterPreview CurrentBaseScene() const;

    SemaphoreHandle_t state_mutex_ = nullptr;
    TaskHandle_t render_task_ = nullptr;
    gfx_obj_t* character_image_ = nullptr;
    uint8_t* frame_buffers_[2] = {nullptr, nullptr};
    uint8_t* cover_buffer_ = nullptr;
    gfx_image_dsc_t frame_descriptors_[2] = {};
    gfx_image_dsc_t cover_descriptor_ = {};
    anim::CharacterActionRequest action_request_;
    std::atomic<bool> stopping_{false};
    std::atomic<bool> music_active_{false};
    std::atomic<bool> cover_ready_{false};
    std::atomic<bool> idle_{false};
    std::atomic<int> status_scene_{static_cast<int>(anim::CharacterPreview::kStartup)};
    std::atomic<int> emotion_scene_{static_cast<int>(anim::CharacterPreview::kEyes)};
    std::atomic<int> companion_preference_{1};
    std::atomic<int64_t> music_started_us_{0};
};

}  // namespace emote
