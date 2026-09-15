#pragma once

#include "display/lcd_display.h"
#include <memory>
#include <optional>
#include <functional>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include "mmap_generate_emoji_normal.h"
#include "gfx.h"
#include <atomic>
#include <array>
#include <cstdint>
#include "character_preview.h"
#include "music_companion_clock.h"

namespace anim {

// Helper function for setting up image descriptors
bool SetupImageDescriptor(mmap_assets_handle_t assets_handle, gfx_image_dsc_t* img_dsc, int asset_id);

class EmoteEngine;
class ExpressionDirector;
struct ExpressionRenderModel;

using FlushIoReadyCallback = std::function<bool(esp_lcd_panel_io_handle_t, esp_lcd_panel_io_event_data_t*, void*)>;
using FlushCallback = std::function<void(gfx_handle_t, int, int, int, int, const void*)>;

class EmoteEngine {
public:
    EmoteEngine(esp_lcd_panel_handle_t panel, esp_lcd_panel_io_handle_t panel_io);
    ~EmoteEngine();

    void setEyes(int aaf, bool repeat, int fps);
    void stopEyes();
#if CONFIG_ECHOEAR_CHARACTER_PREVIEW
    bool BeginCharacterPreview(CharacterPreview initial = CharacterPreview::kEyes, bool guitar_cache = true,
                               bool companion = false, double seconds = 0);
    bool DrawCharacterPreview(CharacterPreview scene, double seconds);
    void EndCharacterPreview();
#endif
    
    void Lock();
    void Unlock();
    
    void SetIcon(int asset_id);
    void EnterMusicScene(const MusicTrackInfo& track);
    void UpdateMusicTrackInfo(const MusicTrackInfo& track);
    void SetMusicArtwork(const uint16_t* background, int background_width,
                         int background_height, const uint16_t* disc,
                         int disc_width, int disc_height);
    void CommitMusicFallback();
    void SetMusicLyrics(const std::string& previous, const std::string& current,
                        const std::string& next);
    void SetMusicProgress(int position_ms, int duration_ms);
    void SetMusicOverlayVisible(bool visible);
    void ExitMusicScene();
    bool IsMusicSceneActive() const { return music_scene_active_; }
    bool IsMusicOverlayVisible() const { return music_overlay_visible_; }
    bool IsMusicCompanionVisible() const { return music_companion_visible_; }
    mmap_assets_handle_t GetAssetsHandle() const { return assets_handle_; }

    // Callback functions (public to be accessible from static helper functions)
    static bool OnFlushIoReady(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_io_event_data_t *edata, void *user_ctx);
    static void OnFlush(gfx_handle_t handle, int x_start, int y_start, int x_end, int y_end, const void *color_data);

private:
    gfx_handle_t engine_handle_;
#if CONFIG_ECHOEAR_CHARACTER_PREVIEW
    gfx_obj_t* character_image_ = nullptr;
    uint8_t* character_front_ = nullptr;
    uint8_t* character_back_ = nullptr;
    uint8_t* character_guitar_base_ = nullptr;
    bool character_guitar_cache_attempted_ = false;
    gfx_image_dsc_t character_descriptor_{};
#endif
    mmap_assets_handle_t assets_handle_;
    uint8_t* music_background_data_ = nullptr;
    uint8_t* music_disc_source_ = nullptr;
    uint8_t* music_disc_frame_ = nullptr;
    uint8_t* music_disc_back_frame_ = nullptr;
    gfx_image_dsc_t music_background_dsc_{};
    gfx_image_dsc_t music_disc_dsc_{};
    esp_timer_handle_t music_rotation_timer_ = nullptr;
    esp_timer_handle_t music_fallback_timer_ = nullptr;
    esp_timer_handle_t music_release_timer_ = nullptr;
    TaskHandle_t music_rotation_task_ = nullptr;
    std::atomic<bool> music_rotation_task_stopping_{false};
    std::atomic<bool> music_rotation_paused_{true};
    std::atomic<bool> music_rotation_busy_{false};
    std::atomic<bool> music_scene_active_{false};
    std::atomic<bool> music_companion_visible_{false};
    // requested is the director's desired foreground ownership; visible is
    // the state actually committed to the panel after artwork is ready.
    std::atomic<bool> music_overlay_requested_{false};
    std::atomic<bool> music_overlay_visible_{false};
    std::atomic<bool> music_artwork_ready_{false};
    std::atomic<int64_t> music_scene_started_us_{0};
    size_t music_scene_internal_free_before_ = 0;
    size_t music_scene_spiram_free_before_ = 0;
    float music_disc_angle_ = 0.0f;
    std::array<int16_t, 192> music_disc_left_{};
    std::array<int16_t, 192> music_disc_right_{};

    void ClearMusicArtworkLocked();
    bool CreateFallbackBackgroundLocked();
    void CreateFallbackDiscLocked();
    void CommitMusicSceneLocked();
    void InitializeMusicDiscBuffer(uint8_t* buffer);
    void WaitForMusicRotationIdle();
    void RotateMusicDisc();
    static void MusicRotationTimer(void* arg);
    static void MusicFallbackTimer(void* arg);
    static void MusicReleaseTimer(void* arg);
    static void MusicRotationTask(void* arg);
};

class EmoteDisplay : public Display {
public:
    EmoteDisplay(esp_lcd_panel_handle_t panel, esp_lcd_panel_io_handle_t panel_io);
    virtual ~EmoteDisplay();

    virtual void SetBehavior(const DisplayBehaviorRequest& request) override;
    virtual void SetEmotion(const char* emotion) override;
    virtual void SetStatus(const char* status) override;
    virtual void SetChatMessage(const char* role, const char* content) override;
    virtual void EnterMusicScene(const MusicTrackInfo& track) override;
    virtual void UpdateMusicTrackInfo(const MusicTrackInfo& track) override;
    virtual void SetMusicArtwork(const uint16_t* background, int background_width,
                                 int background_height, const uint16_t* disc,
                                 int disc_width, int disc_height) override;
    virtual void CommitMusicFallback() override;
    virtual void SetMusicLyricWindow(const std::string& previous,
                                     const std::string& current,
                                     const std::string& next) override;
    virtual void UpdateMusicProgress(int position_ms, int duration_ms) override;
    virtual void ExitMusicScene() override;
    virtual bool SupportsExpressionTest() const override { return true; }
    virtual bool StartExpressionTest() override;
    void CancelExpressionTest() override;
    
    anim::EmoteEngine* GetEngine()
    {
        return engine_.get();
    }

private:
    void InitializeEngine(esp_lcd_panel_handle_t panel, esp_lcd_panel_io_handle_t panel_io);
    void InitializeDirector();
    void ApplyRenderModel(const ExpressionRenderModel& render_model);
    void ApplyExpressionTestFrame(const char* name, const ExpressionRenderModel& render_model);
    void RunExpressionTest();
    bool StartCharacterTest(CharacterTestSuite suite);
    CharacterTestSuite character_test_suite_ = CharacterTestSuite::kBaseline; // immutable during test
    static void ExpressionTestTask(void* arg);
#if CONFIG_ECHOEAR_CHARACTER_LIVE_TRIAL
    static void LiveCharacterTask(void* arg);
    void StopLiveCharacter();
    SemaphoreHandle_t live_mutex_ = nullptr;
    TaskHandle_t live_task_ = nullptr;
    std::atomic<bool> live_shutdown_{false};
    std::atomic<bool> live_failed_{false};
    std::optional<CharacterPreview> live_pose_; // protected by live_mutex_
    bool live_owns_preview_ = false;
    int64_t live_started_us_ = 0;
    bool live_companion_ = false;
    bool companion_redraw_ = false;
    CharacterPreview companion_instrument_ = CharacterPreview::kShaker;
    bool companion_advancing_ = false;
    MusicCompanionClock companion_clock_;
    int64_t companion_stats_since_us_ = 0;
    uint32_t companion_frames_ = 0;
    int64_t companion_render_us_ = 0, companion_max_us_ = 0;
#endif
#if CONFIG_ECHOEAR_CHARACTER_TEST_SERIAL
    static void CharacterSerialTask(void* arg);
#endif
    virtual bool Lock(int timeout_ms = 0) override;
    virtual void Unlock() override;

    std::unique_ptr<anim::EmoteEngine> engine_;
    std::unique_ptr<anim::ExpressionDirector> director_;
    std::atomic<bool> expression_test_running_{false};
    std::atomic<bool> character_test_cancelled_{false};
    // Suppress stale speaking/listening callbacks during the short hand-off
    // between EnterMusicScene() and the first authoritative music behavior.
    std::atomic<bool> music_scene_behavior_ready_{false};
};

} // namespace anim
