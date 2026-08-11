#pragma once

#include <esp_timer.h>

#include <cstdint>
#include <functional>
#include <optional>
#include <string>

#include "display/display_behavior.h"

namespace anim {

enum class ExpressionUiMode {
    kTips,
    kTime,
    kListening,
};

struct ExpressionRenderModel {
    int animation_asset_id;
    bool repeat;
    int fps;
    int icon_asset_id;
    ExpressionUiMode ui_mode;

    bool operator==(const ExpressionRenderModel& other) const;
};

class ExpressionDirector {
public:
    using RenderCallback = std::function<void(const ExpressionRenderModel&)>;

    explicit ExpressionDirector(RenderCallback render_callback);
    ~ExpressionDirector();

    void SetBaseBehavior(const DisplayBehaviorRequest& request);
    void SetMediaBehavior(const DisplayBehaviorRequest& request);
    void ClearMediaBehavior();
    void PostTransientBehavior(const DisplayBehaviorRequest& request);
    void SetCloudEmotion(const char* emotion);
    void ForceRender();

private:
    struct BehaviorState {
        DisplayBehaviorRequest request;
        int priority;
        int64_t expires_at_us;
    };

    struct EmotionState {
        std::string name;
        ExpressionRenderModel render_model;
        int64_t expires_at_us;
    };

    struct IdleMotionState {
        const char* name;
        ExpressionRenderModel render_model;
        int64_t expires_at_us;
    };

    static void TimerCallback(void* arg);
    void OnTimer();
    void HandleDeadline();
    void Recompute(const char* reason);
    void ScheduleNextDeadline();
    void StartIdleTimeline(int64_t now);
    void StopIdleTimeline();
    void UpdateIdleState(int64_t now);
    bool IsIdleEligible() const;
    int64_t GetRandomIdleIntervalUs() const;

    static int GetPriority(DisplayBehavior behavior);
    static const char* GetBehaviorName(DisplayBehavior behavior);
    static ExpressionRenderModel GetRenderModel(DisplayBehavior behavior);
    static std::optional<ExpressionRenderModel> GetEmotionRenderModel(const char* emotion);

    RenderCallback render_callback_;
    DisplayBehaviorRequest base_behavior_ = {
        DisplayBehavior::kStartup,
        DisplayBehaviorSource::kDeviceState,
        {},
        0,
    };
    std::optional<BehaviorState> media_behavior_;
    std::optional<BehaviorState> transient_behavior_;
    std::optional<EmotionState> cloud_emotion_;
    std::optional<IdleMotionState> idle_motion_;
    std::optional<ExpressionRenderModel> active_render_model_;
    DisplayBehavior active_behavior_ = DisplayBehavior::kStartup;
    int64_t thinking_visible_until_us_ = 0;
    int64_t idle_started_at_us_ = 0;
    int64_t next_idle_motion_at_us_ = INT64_MAX;
    int last_idle_motion_index_ = -1;
    bool idle_sleeping_ = false;
    esp_timer_handle_t timer_ = nullptr;
};

}  // namespace anim
