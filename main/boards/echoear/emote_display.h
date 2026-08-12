#pragma once

#include "display/lcd_display.h"
#include <memory>
#include <functional>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <esp_timer.h>
#include "mmap_generate_emoji_normal.h"
#include "gfx.h"
#include <atomic>
#include <cstdint>

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
    
    void Lock();
    void Unlock();
    
    void SetIcon(int asset_id);
    void EnterMusicScene(const MusicTrackInfo& track);
    void SetMusicArtwork(const uint16_t* background, int background_width,
                         int background_height, const uint16_t* disc,
                         int disc_width, int disc_height);
    void SetMusicLyrics(const std::string& previous, const std::string& current,
                        const std::string& next);
    void SetMusicProgress(int position_ms, int duration_ms);
    void SetMusicOverlayVisible(bool visible);
    void ExitMusicScene();
    bool IsMusicSceneActive() const { return music_scene_active_; }
    mmap_assets_handle_t GetAssetsHandle() const { return assets_handle_; }

    // Callback functions (public to be accessible from static helper functions)
    static bool OnFlushIoReady(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_io_event_data_t *edata, void *user_ctx);
    static void OnFlush(gfx_handle_t handle, int x_start, int y_start, int x_end, int y_end, const void *color_data);

private:
    gfx_handle_t engine_handle_;
    mmap_assets_handle_t assets_handle_;
    uint8_t* music_background_data_ = nullptr;
    uint8_t* music_disc_source_ = nullptr;
    uint8_t* music_disc_frame_ = nullptr;
    gfx_image_dsc_t music_background_dsc_{};
    gfx_image_dsc_t music_disc_dsc_{};
    esp_timer_handle_t music_rotation_timer_ = nullptr;
    std::atomic<bool> music_scene_active_{false};
    float music_disc_angle_ = 0.0f;

    void ClearMusicArtworkLocked();
    void CreateFallbackDiscLocked();
    void RotateMusicDisc();
    static void MusicRotationTimer(void* arg);
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
    virtual void SetMusicArtwork(const uint16_t* background, int background_width,
                                 int background_height, const uint16_t* disc,
                                 int disc_width, int disc_height) override;
    virtual void SetMusicLyricWindow(const std::string& previous,
                                     const std::string& current,
                                     const std::string& next) override;
    virtual void UpdateMusicProgress(int position_ms, int duration_ms) override;
    virtual void ExitMusicScene() override;
    virtual bool SupportsExpressionTest() const override { return true; }
    virtual bool StartExpressionTest() override;
    
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
    static void ExpressionTestTask(void* arg);
    virtual bool Lock(int timeout_ms = 0) override;
    virtual void Unlock() override;

    std::unique_ptr<anim::EmoteEngine> engine_;
    std::unique_ptr<anim::ExpressionDirector> director_;
    std::atomic<bool> expression_test_running_{false};
};

} // namespace anim
