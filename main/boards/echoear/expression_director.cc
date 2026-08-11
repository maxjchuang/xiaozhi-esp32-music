#include "expression_director.h"

#include <algorithm>
#include <cstring>
#include <utility>

#include <esp_log.h>

#include "application.h"
#include "mmap_generate_emoji_normal.h"

namespace anim {

namespace {

constexpr char TAG[] = "Expression";
constexpr int64_t kCloudEmotionDurationUs = 5 * 1000 * 1000;
constexpr int64_t kThinkingMinimumVisibleUs = 600 * 1000;

constexpr int kPriorityIdle = 0;
constexpr int kPriorityEmotion = 1;
constexpr int kPriorityMedia = 2;
constexpr int kPriorityInput = 3;
constexpr int kPriorityTask = 4;
constexpr int kPriorityOutput = 5;
constexpr int kPriorityCritical = 6;

}  // namespace

bool ExpressionRenderModel::operator==(const ExpressionRenderModel& other) const
{
    return animation_asset_id == other.animation_asset_id &&
           repeat == other.repeat &&
           fps == other.fps &&
           icon_asset_id == other.icon_asset_id &&
           ui_mode == other.ui_mode;
}

ExpressionDirector::ExpressionDirector(RenderCallback render_callback)
    : render_callback_(std::move(render_callback))
{
    const esp_timer_create_args_t timer_args = {
        .callback = TimerCallback,
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "expression",
        .skip_unhandled_events = true,
    };
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &timer_));
    Recompute("initialize");
}

ExpressionDirector::~ExpressionDirector()
{
    if (timer_ != nullptr) {
        esp_timer_stop(timer_);
        esp_timer_delete(timer_);
    }
}

void ExpressionDirector::SetBaseBehavior(const DisplayBehaviorRequest& request)
{
    base_behavior_ = request;
    // STT and TTS can arrive only a few milliseconds apart. Keep thinking on
    // screen long enough to be perceived, without delaying audio playback.
    if (request.behavior == DisplayBehavior::kSpeaking &&
        transient_behavior_.has_value() &&
        transient_behavior_->request.behavior == DisplayBehavior::kThinking) {
        const int64_t now = esp_timer_get_time();
        if (now < thinking_visible_until_us_) {
            transient_behavior_->expires_at_us = thinking_visible_until_us_;
        } else {
            transient_behavior_.reset();
        }
    }
    Recompute("base_state");
}

void ExpressionDirector::SetMediaBehavior(const DisplayBehaviorRequest& request)
{
    media_behavior_ = BehaviorState{
        request,
        GetPriority(request.behavior),
        INT64_MAX,
    };
    Recompute("media_state");
}

void ExpressionDirector::ClearMediaBehavior()
{
    if (!media_behavior_.has_value()) {
        return;
    }
    media_behavior_.reset();
    Recompute("media_stopped");
}

void ExpressionDirector::PostTransientBehavior(const DisplayBehaviorRequest& request)
{
    const int duration_ms = request.duration_ms > 0 ? request.duration_ms : 1000;
    const int priority = GetPriority(request.behavior);
    const int64_t now = esp_timer_get_time();

    if (transient_behavior_.has_value() &&
        transient_behavior_->expires_at_us > now &&
        transient_behavior_->priority > priority) {
        ESP_LOGW(TAG, "%s ignored active=%s reason=lower_priority",
                 GetBehaviorName(request.behavior),
                 GetBehaviorName(transient_behavior_->request.behavior));
        return;
    }

    transient_behavior_ = BehaviorState{
        request,
        priority,
        now + static_cast<int64_t>(duration_ms) * 1000,
    };
    if (request.behavior == DisplayBehavior::kThinking) {
        thinking_visible_until_us_ = now + kThinkingMinimumVisibleUs;
    }
    Recompute("transient");
}

void ExpressionDirector::SetCloudEmotion(const char* emotion)
{
    if (emotion == nullptr || emotion[0] == '\0') {
        return;
    }

    auto render_model = GetEmotionRenderModel(emotion);
    if (!render_model.has_value()) {
        ESP_LOGW(TAG, "Unknown cloud emotion ignored: %s", emotion);
        return;
    }

    cloud_emotion_ = EmotionState{
        emotion,
        *render_model,
        esp_timer_get_time() + kCloudEmotionDurationUs,
    };
    Recompute("cloud_emotion");
}

void ExpressionDirector::ForceRender()
{
    active_render_model_.reset();
    Recompute("force_render");
}

void ExpressionDirector::TimerCallback(void* arg)
{
    static_cast<ExpressionDirector*>(arg)->OnTimer();
}

void ExpressionDirector::OnTimer()
{
    Application::GetInstance().Schedule([this]() {
        HandleDeadline();
    });
}

void ExpressionDirector::HandleDeadline()
{
    const int64_t now = esp_timer_get_time();
    if (transient_behavior_.has_value() && transient_behavior_->expires_at_us <= now) {
        ESP_LOGI(TAG, "%s expired", GetBehaviorName(transient_behavior_->request.behavior));
        transient_behavior_.reset();
    }
    if (cloud_emotion_.has_value() && cloud_emotion_->expires_at_us <= now) {
        ESP_LOGI(TAG, "cloud_%s expired", cloud_emotion_->name.c_str());
        cloud_emotion_.reset();
    }
    Recompute("deadline");
}

void ExpressionDirector::Recompute(const char* reason)
{
    const int64_t now = esp_timer_get_time();
    if (transient_behavior_.has_value() && transient_behavior_->expires_at_us <= now) {
        transient_behavior_.reset();
    }
    if (cloud_emotion_.has_value() && cloud_emotion_->expires_at_us <= now) {
        cloud_emotion_.reset();
    }

    DisplayBehavior next_behavior = base_behavior_.behavior;
    int next_priority = GetPriority(base_behavior_.behavior);
    ExpressionRenderModel next_render_model = GetRenderModel(base_behavior_.behavior);
    const char* selected_source = "base";

    if (cloud_emotion_.has_value() && kPriorityEmotion > next_priority) {
        next_priority = kPriorityEmotion;
        next_render_model = cloud_emotion_->render_model;
        selected_source = "cloud";
    }

    if (media_behavior_.has_value() && media_behavior_->priority > next_priority) {
        next_behavior = media_behavior_->request.behavior;
        next_priority = media_behavior_->priority;
        next_render_model = GetRenderModel(next_behavior);
        selected_source = "media";
    }

    const bool preserve_thinking_lead_in =
        transient_behavior_.has_value() &&
        transient_behavior_->request.behavior == DisplayBehavior::kThinking &&
        base_behavior_.behavior == DisplayBehavior::kSpeaking &&
        now < thinking_visible_until_us_;
    if (transient_behavior_.has_value() &&
        (preserve_thinking_lead_in || transient_behavior_->priority >= next_priority)) {
        next_behavior = transient_behavior_->request.behavior;
        next_priority = transient_behavior_->priority;
        next_render_model = GetRenderModel(next_behavior);
        selected_source = "transient";
    }

    if (!active_render_model_.has_value() || !(*active_render_model_ == next_render_model)) {
        ESP_LOGI(TAG, "%s -> %s source=%s reason=%s priority=%d",
                 GetBehaviorName(active_behavior_), GetBehaviorName(next_behavior),
                 selected_source, reason, next_priority);
        active_behavior_ = next_behavior;
        active_render_model_ = next_render_model;
        render_callback_(next_render_model);
    }

    ScheduleNextDeadline();
}

void ExpressionDirector::ScheduleNextDeadline()
{
    if (timer_ == nullptr) {
        return;
    }

    esp_timer_stop(timer_);

    int64_t next_deadline = INT64_MAX;
    if (transient_behavior_.has_value()) {
        next_deadline = std::min(next_deadline, transient_behavior_->expires_at_us);
    }
    if (cloud_emotion_.has_value()) {
        next_deadline = std::min(next_deadline, cloud_emotion_->expires_at_us);
    }
    if (next_deadline == INT64_MAX) {
        return;
    }

    const int64_t delay_us = std::max<int64_t>(1, next_deadline - esp_timer_get_time());
    ESP_ERROR_CHECK(esp_timer_start_once(timer_, delay_us));
}

int ExpressionDirector::GetPriority(DisplayBehavior behavior)
{
    switch (behavior) {
    case DisplayBehavior::kIdle:
        return kPriorityIdle;
    case DisplayBehavior::kMusicBuffering:
    case DisplayBehavior::kMusicPlaying:
    case DisplayBehavior::kMusicPaused:
        return kPriorityMedia;
    case DisplayBehavior::kStartup:
    case DisplayBehavior::kConnecting:
    case DisplayBehavior::kThinking:
    case DisplayBehavior::kToolRunning:
    case DisplayBehavior::kSuccess:
    case DisplayBehavior::kRecoverableError:
        return kPriorityTask;
    case DisplayBehavior::kSpeaking:
        return kPriorityOutput;
    case DisplayBehavior::kWakeAcknowledged:
    case DisplayBehavior::kListening:
        return kPriorityInput;
    case DisplayBehavior::kFatalError:
        return kPriorityCritical;
    }

    return kPriorityIdle;
}

const char* ExpressionDirector::GetBehaviorName(DisplayBehavior behavior)
{
    switch (behavior) {
    case DisplayBehavior::kStartup:
        return "startup";
    case DisplayBehavior::kConnecting:
        return "connecting";
    case DisplayBehavior::kIdle:
        return "idle";
    case DisplayBehavior::kWakeAcknowledged:
        return "wake_acknowledged";
    case DisplayBehavior::kListening:
        return "listening";
    case DisplayBehavior::kThinking:
        return "thinking";
    case DisplayBehavior::kToolRunning:
        return "tool_running";
    case DisplayBehavior::kSpeaking:
        return "speaking";
    case DisplayBehavior::kSuccess:
        return "success";
    case DisplayBehavior::kRecoverableError:
        return "recoverable_error";
    case DisplayBehavior::kFatalError:
        return "fatal_error";
    case DisplayBehavior::kMusicBuffering:
        return "music_buffering";
    case DisplayBehavior::kMusicPlaying:
        return "music_playing";
    case DisplayBehavior::kMusicPaused:
        return "music_paused";
    }

    return "unknown";
}

ExpressionRenderModel ExpressionDirector::GetRenderModel(DisplayBehavior behavior)
{
    switch (behavior) {
    case DisplayBehavior::kStartup:
        return {MMAP_EMOJI_NORMAL_NEUTRAL_EAF, true, 20,
                MMAP_EMOJI_NORMAL_ICON_WIFI_BIN, ExpressionUiMode::kTips};
    case DisplayBehavior::kConnecting:
        return {MMAP_EMOJI_NORMAL_CONFUSED_EAF, true, 20,
                MMAP_EMOJI_NORMAL_ICON_WIFI_BIN, ExpressionUiMode::kTips};
    case DisplayBehavior::kIdle:
        return {MMAP_EMOJI_NORMAL_NEUTRAL_EAF, true, 20,
                MMAP_EMOJI_NORMAL_ICON_BATTERY_BIN, ExpressionUiMode::kTime};
    case DisplayBehavior::kWakeAcknowledged:
        return {MMAP_EMOJI_NORMAL_WINKING_EAF, false, 20,
                MMAP_EMOJI_NORMAL_ICON_MIC_BIN, ExpressionUiMode::kListening};
    case DisplayBehavior::kListening:
        return {MMAP_EMOJI_NORMAL_NEUTRAL_EAF, true, 20,
                MMAP_EMOJI_NORMAL_ICON_MIC_BIN, ExpressionUiMode::kListening};
    case DisplayBehavior::kThinking:
    case DisplayBehavior::kToolRunning:
    case DisplayBehavior::kMusicBuffering:
        return {MMAP_EMOJI_NORMAL_CONFUSED_EAF, true, 20,
                MMAP_EMOJI_NORMAL_ICON_SPEAKER_ZZZ_BIN, ExpressionUiMode::kTips};
    case DisplayBehavior::kSpeaking:
        return {MMAP_EMOJI_NORMAL_HAPPY_EAF, true, 20,
                MMAP_EMOJI_NORMAL_ICON_SPEAKER_ZZZ_BIN, ExpressionUiMode::kTips};
    case DisplayBehavior::kSuccess:
        return {MMAP_EMOJI_NORMAL_WINKING_EAF, false, 20,
                MMAP_EMOJI_NORMAL_ICON_SPEAKER_ZZZ_BIN, ExpressionUiMode::kTips};
    case DisplayBehavior::kRecoverableError:
        return {MMAP_EMOJI_NORMAL_CRY_EAF, true, 20,
                MMAP_EMOJI_NORMAL_ICON_WIFI_FAILED_BIN, ExpressionUiMode::kTips};
    case DisplayBehavior::kFatalError:
        return {MMAP_EMOJI_NORMAL_SHOCKED_EAF, true, 20,
                MMAP_EMOJI_NORMAL_ICON_WIFI_FAILED_BIN, ExpressionUiMode::kTips};
    case DisplayBehavior::kMusicPlaying:
        return {MMAP_EMOJI_NORMAL_HAPPY_EAF, true, 20,
                MMAP_EMOJI_NORMAL_ICON_SPEAKER_ZZZ_BIN, ExpressionUiMode::kTips};
    case DisplayBehavior::kMusicPaused:
        return {MMAP_EMOJI_NORMAL_SLEEP_EAF, true, 16,
                MMAP_EMOJI_NORMAL_ICON_SPEAKER_ZZZ_BIN, ExpressionUiMode::kTips};
    }

    return {MMAP_EMOJI_NORMAL_NEUTRAL_EAF, true, 20,
            MMAP_EMOJI_NORMAL_ICON_BATTERY_BIN, ExpressionUiMode::kTime};
}

std::optional<ExpressionRenderModel> ExpressionDirector::GetEmotionRenderModel(const char* emotion)
{
    if (std::strcmp(emotion, "happy") == 0 ||
        std::strcmp(emotion, "loving") == 0 ||
        std::strcmp(emotion, "confident") == 0 ||
        std::strcmp(emotion, "winking") == 0) {
        return ExpressionRenderModel{MMAP_EMOJI_NORMAL_HAPPY_EAF, true, 20,
                                     MMAP_EMOJI_NORMAL_ICON_BATTERY_BIN, ExpressionUiMode::kTime};
    }
    if (std::strcmp(emotion, "laughing") == 0 ||
        std::strcmp(emotion, "funny") == 0 ||
        std::strcmp(emotion, "delicious") == 0) {
        return ExpressionRenderModel{MMAP_EMOJI_NORMAL_HAPPY_EAF, true, 20,
                                     MMAP_EMOJI_NORMAL_ICON_BATTERY_BIN, ExpressionUiMode::kTime};
    }
    if (std::strcmp(emotion, "sad") == 0) {
        return ExpressionRenderModel{MMAP_EMOJI_NORMAL_SAD_EAF, true, 20,
                                     MMAP_EMOJI_NORMAL_ICON_BATTERY_BIN, ExpressionUiMode::kTime};
    }
    if (std::strcmp(emotion, "crying") == 0) {
        return ExpressionRenderModel{MMAP_EMOJI_NORMAL_CRY_EAF, true, 20,
                                     MMAP_EMOJI_NORMAL_ICON_BATTERY_BIN, ExpressionUiMode::kTime};
    }
    if (std::strcmp(emotion, "angry") == 0) {
        return ExpressionRenderModel{MMAP_EMOJI_NORMAL_ANGRY_EAF, true, 20,
                                     MMAP_EMOJI_NORMAL_ICON_BATTERY_BIN, ExpressionUiMode::kTime};
    }
    if (std::strcmp(emotion, "surprised") == 0 || std::strcmp(emotion, "shocked") == 0) {
        return ExpressionRenderModel{MMAP_EMOJI_NORMAL_SHOCKED_EAF, true, 20,
                                     MMAP_EMOJI_NORMAL_ICON_BATTERY_BIN, ExpressionUiMode::kTime};
    }
    if (std::strcmp(emotion, "thinking") == 0 || std::strcmp(emotion, "embarrassed") == 0) {
        return ExpressionRenderModel{MMAP_EMOJI_NORMAL_CONFUSED_EAF, true, 20,
                                     MMAP_EMOJI_NORMAL_ICON_BATTERY_BIN, ExpressionUiMode::kTime};
    }
    if (std::strcmp(emotion, "silly") == 0 || std::strcmp(emotion, "confused") == 0) {
        return ExpressionRenderModel{MMAP_EMOJI_NORMAL_CONFUSED_EAF, true, 20,
                                     MMAP_EMOJI_NORMAL_ICON_BATTERY_BIN, ExpressionUiMode::kTime};
    }
    if (std::strcmp(emotion, "sleepy") == 0) {
        return ExpressionRenderModel{MMAP_EMOJI_NORMAL_SLEEP_EAF, true, 16,
                                     MMAP_EMOJI_NORMAL_ICON_BATTERY_BIN, ExpressionUiMode::kTime};
    }
    if (std::strcmp(emotion, "relaxed") == 0 ||
        std::strcmp(emotion, "neutral") == 0 ||
        std::strcmp(emotion, "idle") == 0) {
        return ExpressionRenderModel{MMAP_EMOJI_NORMAL_NEUTRAL_EAF, true, 20,
                                     MMAP_EMOJI_NORMAL_ICON_BATTERY_BIN, ExpressionUiMode::kTime};
    }

    return std::nullopt;
}

}  // namespace anim
