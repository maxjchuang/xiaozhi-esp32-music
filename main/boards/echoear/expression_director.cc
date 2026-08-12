#include "expression_director.h"

#include <algorithm>
#include <cstring>
#include <utility>

#include <esp_log.h>
#include <esp_random.h>

#include "application.h"
#include "mmap_generate_emoji_normal.h"

namespace anim {

namespace {

constexpr char TAG[] = "Expression";
constexpr int64_t kCloudEmotionDurationUs = 5 * 1000 * 1000;
constexpr int64_t kThinkingMinimumVisibleUs = 600 * 1000;
constexpr int64_t kIdleMinimumStableUs = 5 * 1000 * 1000;
constexpr int64_t kIdleMotionMinIntervalUs = 8 * 1000 * 1000;
constexpr int64_t kIdleMotionMaxIntervalUs = 25 * 1000 * 1000;

constexpr int kPriorityIdle = 0;
constexpr int kPriorityEmotion = 1;
constexpr int kPriorityMedia = 2;
constexpr int kPriorityTask = 3;
constexpr int kPriorityOutput = 4;
constexpr int kPriorityInput = 5;
constexpr int kPriorityCritical = 6;

}  // namespace

bool ExpressionRenderModel::operator==(const ExpressionRenderModel& other) const
{
    return animation_asset_id == other.animation_asset_id &&
           repeat == other.repeat &&
           fps == other.fps &&
           icon_asset_id == other.icon_asset_id &&
           ui_mode == other.ui_mode &&
           text == other.text;
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
    const esp_err_t timer_result = esp_timer_create(&timer_args, &timer_);
    if (timer_result != ESP_OK) {
        timer_ = nullptr;
        ESP_LOGE(TAG, "Expression timer unavailable: %s; timed effects disabled",
                 esp_err_to_name(timer_result));
    }
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
    const bool was_idle = base_behavior_.behavior == DisplayBehavior::kIdle;
    base_behavior_ = request;
    const int64_t now = esp_timer_get_time();
    if (request.behavior == DisplayBehavior::kIdle) {
        if (!was_idle) {
            StartIdleTimeline(now);
        }
    } else {
        StopIdleTimeline();
    }
    // STT and TTS can arrive only a few milliseconds apart. Keep thinking on
    // screen long enough to be perceived, without delaying audio playback.
    if (request.behavior == DisplayBehavior::kSpeaking &&
        transient_behavior_.has_value() &&
        transient_behavior_->request.behavior == DisplayBehavior::kThinking) {
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
    StopIdleTimeline();
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
    if (base_behavior_.behavior == DisplayBehavior::kIdle) {
        StartIdleTimeline(esp_timer_get_time());
    }
    Recompute("media_stopped");
}

void ExpressionDirector::PostTransientBehavior(const DisplayBehaviorRequest& request)
{
    const int duration_ms = request.duration_ms > 0 ? request.duration_ms : 1000;
    const int priority = GetPriority(request.behavior);
    const int64_t now = esp_timer_get_time();

    if (base_behavior_.behavior == DisplayBehavior::kIdle) {
        StartIdleTimeline(now);
    }

    if (transient_behavior_.has_value() &&
        transient_behavior_->expires_at_us > now &&
        transient_behavior_->priority > priority) {
        ESP_LOGW(TAG, "%s ignored active=%s reason=lower_priority priority=%d active_priority=%d",
                 GetBehaviorName(request.behavior),
                 GetBehaviorName(transient_behavior_->request.behavior),
                 priority, transient_behavior_->priority);
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

    const int64_t now = esp_timer_get_time();
    if (base_behavior_.behavior == DisplayBehavior::kIdle) {
        StartIdleTimeline(now);
    }
    cloud_emotion_ = EmotionState{
        emotion,
        *render_model,
        now + kCloudEmotionDurationUs,
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
    if (idle_motion_.has_value() && idle_motion_->expires_at_us <= now) {
#if CONFIG_ECHOEAR_EXPRESSION_DEBUG_LOG
        ESP_LOGI(TAG, "Idle motion finished: %s", idle_motion_->name);
#endif
        idle_motion_.reset();
        next_idle_motion_at_us_ = now + GetRandomIdleIntervalUs();
    }
    UpdateIdleState(now);

    DisplayBehavior next_behavior = base_behavior_.behavior;
    int next_priority = GetPriority(base_behavior_.behavior);
    ExpressionRenderModel next_render_model = GetRenderModel(base_behavior_.behavior,
                                                              base_behavior_.detail);
    const char* selected_source = GetSourceName(base_behavior_.source);
    std::string selected_detail = base_behavior_.detail;
    int64_t selected_expires_at_us = INT64_MAX;

    if (base_behavior_.behavior == DisplayBehavior::kIdle &&
        !media_behavior_.has_value() &&
        !transient_behavior_.has_value() &&
        !cloud_emotion_.has_value()) {
        if (idle_sleeping_) {
            next_render_model = {MMAP_EMOJI_NORMAL_SLEEP_EAF, true, 16,
                                 MMAP_EMOJI_NORMAL_ICON_BATTERY_BIN,
                                 ExpressionUiMode::kImmersive};
            selected_source = "timer_sleep";
        } else if (idle_motion_.has_value()) {
            next_render_model = idle_motion_->render_model;
            selected_source = idle_motion_->name;
            selected_expires_at_us = idle_motion_->expires_at_us;
        }
    }

    if (cloud_emotion_.has_value() && kPriorityEmotion > next_priority) {
        next_priority = kPriorityEmotion;
        next_render_model = cloud_emotion_->render_model;
        selected_source = "cloud";
        selected_expires_at_us = cloud_emotion_->expires_at_us;
    }

    if (media_behavior_.has_value() && media_behavior_->priority > next_priority) {
        next_behavior = media_behavior_->request.behavior;
        next_priority = media_behavior_->priority;
        next_render_model = GetRenderModel(next_behavior, media_behavior_->request.detail);
        selected_source = GetSourceName(media_behavior_->request.source);
        selected_detail = media_behavior_->request.detail;
        selected_expires_at_us = media_behavior_->expires_at_us;
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
        next_render_model = GetRenderModel(next_behavior, transient_behavior_->request.detail);
        selected_source = GetSourceName(transient_behavior_->request.source);
        selected_detail = transient_behavior_->request.detail;
        selected_expires_at_us = transient_behavior_->expires_at_us;
    }

    // MCP feedback must remain visible even while the higher-priority listening
    // or speaking eyes stay active. Treat it as a text overlay instead of
    // allowing a task animation to interrupt user input or TTS output.
    if (transient_behavior_.has_value() &&
        transient_behavior_->request.source == DisplayBehaviorSource::kMcp &&
        transient_behavior_->priority < next_priority &&
        !transient_behavior_->request.detail.empty()) {
        next_render_model.text = transient_behavior_->request.detail;
        next_render_model.ui_mode = ExpressionUiMode::kTips;
        selected_source = "mcp_overlay";
        selected_detail = transient_behavior_->request.detail;
        selected_expires_at_us = transient_behavior_->expires_at_us;
    }

    if (!active_render_model_.has_value() || !(*active_render_model_ == next_render_model)) {
        const bool preempt = active_render_model_.has_value() && next_priority > active_priority_;
        const int duration_ms = selected_expires_at_us == INT64_MAX
            ? 0
            : static_cast<int>(std::max<int64_t>(0, (selected_expires_at_us - now) / 1000));
        ESP_LOGI(TAG, "%s -> %s source=%s reason=%s priority=%d preempt=%d duration_ms=%d restore=%s detail=%s",
                 GetBehaviorName(active_behavior_), GetBehaviorName(next_behavior),
                 selected_source, reason, next_priority, preempt,
                 duration_ms,
                 GetBehaviorName(base_behavior_.behavior), selected_detail.c_str());
        active_behavior_ = next_behavior;
        active_priority_ = next_priority;
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

    const esp_err_t stop_result = esp_timer_stop(timer_);
    if (stop_result != ESP_OK && stop_result != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "Failed to stop expression timer: %s", esp_err_to_name(stop_result));
    }

    int64_t next_deadline = INT64_MAX;
    if (transient_behavior_.has_value()) {
        next_deadline = std::min(next_deadline, transient_behavior_->expires_at_us);
    }
    if (cloud_emotion_.has_value()) {
        next_deadline = std::min(next_deadline, cloud_emotion_->expires_at_us);
    }
#if CONFIG_ECHOEAR_IDLE_MICRO_MOTIONS
    if (IsIdleEligible()) {
        const int64_t sleep_at_us = idle_started_at_us_ +
            static_cast<int64_t>(CONFIG_ECHOEAR_IDLE_SLEEP_TIMEOUT_SECONDS) * 1000 * 1000;
        if (!idle_sleeping_ && !idle_motion_.has_value()) {
            next_deadline = std::min(next_deadline, sleep_at_us);
        }
        if (idle_motion_.has_value()) {
            next_deadline = std::min(next_deadline, idle_motion_->expires_at_us);
        } else if (!idle_sleeping_) {
            next_deadline = std::min(next_deadline, next_idle_motion_at_us_);
        }
    }
#endif
    if (next_deadline == INT64_MAX) {
        return;
    }

    const int64_t delay_us = std::max<int64_t>(1, next_deadline - esp_timer_get_time());
    const esp_err_t start_result = esp_timer_start_once(timer_, delay_us);
    if (start_result != ESP_OK) {
        ESP_LOGE(TAG, "Failed to schedule expression deadline: %s; timed effects disabled",
                 esp_err_to_name(start_result));
        esp_timer_delete(timer_);
        timer_ = nullptr;
    }
}

void ExpressionDirector::StartIdleTimeline(int64_t now)
{
#if CONFIG_ECHOEAR_IDLE_MICRO_MOTIONS
    idle_started_at_us_ = now;
    idle_motion_.reset();
    idle_sleeping_ = false;
    last_idle_motion_index_ = -1;
    next_idle_motion_at_us_ = std::max(
        now + kIdleMinimumStableUs,
        now + GetRandomIdleIntervalUs());
#else
    (void)now;
#endif
}

void ExpressionDirector::StopIdleTimeline()
{
    idle_started_at_us_ = 0;
    idle_motion_.reset();
    idle_sleeping_ = false;
    last_idle_motion_index_ = -1;
    next_idle_motion_at_us_ = INT64_MAX;
}

bool ExpressionDirector::IsIdleEligible() const
{
#if CONFIG_ECHOEAR_IDLE_MICRO_MOTIONS
    return base_behavior_.behavior == DisplayBehavior::kIdle &&
           idle_started_at_us_ > 0 &&
           !media_behavior_.has_value() &&
           !transient_behavior_.has_value() &&
           !cloud_emotion_.has_value();
#else
    return false;
#endif
}

int64_t ExpressionDirector::GetRandomIdleIntervalUs() const
{
    const uint32_t span = static_cast<uint32_t>(
        kIdleMotionMaxIntervalUs - kIdleMotionMinIntervalUs + 1);
    return kIdleMotionMinIntervalUs + static_cast<int64_t>(esp_random() % span);
}

void ExpressionDirector::UpdateIdleState(int64_t now)
{
#if CONFIG_ECHOEAR_IDLE_MICRO_MOTIONS
    if (!IsIdleEligible() || idle_sleeping_ || idle_motion_.has_value()) {
        return;
    }

    const int64_t sleep_at_us = idle_started_at_us_ +
        static_cast<int64_t>(CONFIG_ECHOEAR_IDLE_SLEEP_TIMEOUT_SECONDS) * 1000 * 1000;
    if (now >= sleep_at_us) {
        idle_sleeping_ = true;
        next_idle_motion_at_us_ = INT64_MAX;
        ESP_LOGI(TAG, "Idle entered sleepy state after %d seconds",
                 CONFIG_ECHOEAR_IDLE_SLEEP_TIMEOUT_SECONDS);
        return;
    }

    if (now < next_idle_motion_at_us_) {
        return;
    }

    struct IdleMotionPreset {
        const char* name;
        ExpressionRenderModel render_model;
        int duration_ms;
    };
    static const IdleMotionPreset presets[] = {
        {"idle_blink", {MMAP_EMOJI_NORMAL_WINKING_EAF, false, 20,
                        MMAP_EMOJI_NORMAL_ICON_BATTERY_BIN,
                        ExpressionUiMode::kImmersive}, 900},
        {"idle_slow_blink", {MMAP_EMOJI_NORMAL_WINKING_EAF, false, 10,
                             MMAP_EMOJI_NORMAL_ICON_BATTERY_BIN,
                             ExpressionUiMode::kImmersive}, 1500},
        {"idle_double_blink", {MMAP_EMOJI_NORMAL_WINKING_EAF, true, 28,
                               MMAP_EMOJI_NORMAL_ICON_BATTERY_BIN,
                               ExpressionUiMode::kImmersive}, 850},
        {"idle_observe", {MMAP_EMOJI_NORMAL_LISTEN_EAF, false, 14,
                          MMAP_EMOJI_NORMAL_ICON_BATTERY_BIN,
                          ExpressionUiMode::kImmersive}, 1400},
        {"idle_curious", {MMAP_EMOJI_NORMAL_CONFUSED_EAF, true, 12,
                          MMAP_EMOJI_NORMAL_ICON_BATTERY_BIN,
                          ExpressionUiMode::kImmersive}, 1200},
    };
    constexpr int preset_count = sizeof(presets) / sizeof(presets[0]);
    int index = static_cast<int>(esp_random() % preset_count);
    if (preset_count > 1 && index == last_idle_motion_index_) {
        index = (index + 1 + static_cast<int>(esp_random() % (preset_count - 1))) % preset_count;
    }

    last_idle_motion_index_ = index;
    idle_motion_ = IdleMotionState{
        presets[index].name,
        presets[index].render_model,
        now + static_cast<int64_t>(presets[index].duration_ms) * 1000,
    };
#if CONFIG_ECHOEAR_EXPRESSION_DEBUG_LOG
    ESP_LOGI(TAG, "Idle motion started: %s duration=%dms",
             presets[index].name, presets[index].duration_ms);
#endif
#else
    (void)now;
#endif
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

const char* ExpressionDirector::GetSourceName(DisplayBehaviorSource source)
{
    switch (source) {
    case DisplayBehaviorSource::kDeviceState:
        return "device_state";
    case DisplayBehaviorSource::kWakeWord:
        return "wake_word";
    case DisplayBehaviorSource::kTts:
        return "tts";
    case DisplayBehaviorSource::kMcp:
        return "mcp";
    case DisplayBehaviorSource::kMusic:
        return "music";
    case DisplayBehaviorSource::kNetwork:
        return "network";
    case DisplayBehaviorSource::kSystem:
        return "system";
    case DisplayBehaviorSource::kTimer:
        return "timer";
    }
    return "unknown";
}

ExpressionRenderModel ExpressionDirector::GetRenderModel(DisplayBehavior behavior,
                                                         const std::string& text)
{
    switch (behavior) {
    case DisplayBehavior::kStartup:
        return {MMAP_EMOJI_NORMAL_NEUTRAL_EAF, true, 20,
                MMAP_EMOJI_NORMAL_ICON_WIFI_BIN, ExpressionUiMode::kTips, text};
    case DisplayBehavior::kConnecting:
        return {MMAP_EMOJI_NORMAL_CONFUSED_EAF, true, 20,
                MMAP_EMOJI_NORMAL_ICON_WIFI_BIN, ExpressionUiMode::kTips, text};
    case DisplayBehavior::kIdle:
        return {MMAP_EMOJI_NORMAL_NEUTRAL_EAF, true, 20,
                MMAP_EMOJI_NORMAL_ICON_BATTERY_BIN, ExpressionUiMode::kImmersive, text};
    case DisplayBehavior::kWakeAcknowledged:
        return {MMAP_EMOJI_NORMAL_WINKING_EAF, false, 20,
                MMAP_EMOJI_NORMAL_ICON_MIC_BIN, ExpressionUiMode::kListening, text};
    case DisplayBehavior::kListening:
        return {MMAP_EMOJI_NORMAL_NEUTRAL_EAF, true, 20,
                MMAP_EMOJI_NORMAL_ICON_MIC_BIN, ExpressionUiMode::kListening, text};
    case DisplayBehavior::kThinking:
    case DisplayBehavior::kToolRunning:
    case DisplayBehavior::kMusicBuffering:
        return {MMAP_EMOJI_NORMAL_CONFUSED_EAF, true, 20,
                MMAP_EMOJI_NORMAL_ICON_SPEAKER_ZZZ_BIN, ExpressionUiMode::kTips, text};
    case DisplayBehavior::kSpeaking:
        return {MMAP_EMOJI_NORMAL_HAPPY_EAF, true, 20,
                MMAP_EMOJI_NORMAL_ICON_SPEAKER_ZZZ_BIN, ExpressionUiMode::kTips, text};
    case DisplayBehavior::kSuccess:
        return {MMAP_EMOJI_NORMAL_WINKING_EAF, false, 20,
                MMAP_EMOJI_NORMAL_ICON_SPEAKER_ZZZ_BIN, ExpressionUiMode::kTips, text};
    case DisplayBehavior::kRecoverableError:
        return {MMAP_EMOJI_NORMAL_CRY_EAF, true, 20,
                MMAP_EMOJI_NORMAL_ICON_WIFI_FAILED_BIN, ExpressionUiMode::kTips, text};
    case DisplayBehavior::kFatalError:
        return {MMAP_EMOJI_NORMAL_SHOCKED_EAF, true, 20,
                MMAP_EMOJI_NORMAL_ICON_WIFI_FAILED_BIN, ExpressionUiMode::kTips, text};
    case DisplayBehavior::kMusicPlaying:
        return {MMAP_EMOJI_NORMAL_HAPPY_EAF, true, 20,
                MMAP_EMOJI_NORMAL_ICON_SPEAKER_ZZZ_BIN, ExpressionUiMode::kTips, text};
    case DisplayBehavior::kMusicPaused:
        return {MMAP_EMOJI_NORMAL_SLEEP_EAF, true, 16,
                MMAP_EMOJI_NORMAL_ICON_SPEAKER_ZZZ_BIN, ExpressionUiMode::kTips, text};
    }

    return {MMAP_EMOJI_NORMAL_NEUTRAL_EAF, true, 20,
            MMAP_EMOJI_NORMAL_ICON_BATTERY_BIN, ExpressionUiMode::kImmersive, text};
}

std::optional<ExpressionRenderModel> ExpressionDirector::GetEmotionRenderModel(const char* emotion)
{
    if (std::strcmp(emotion, "happy") == 0 ||
        std::strcmp(emotion, "loving") == 0 ||
        std::strcmp(emotion, "confident") == 0 ||
        std::strcmp(emotion, "winking") == 0) {
        return ExpressionRenderModel{MMAP_EMOJI_NORMAL_HAPPY_EAF, true, 20,
                                     MMAP_EMOJI_NORMAL_ICON_BATTERY_BIN, ExpressionUiMode::kImmersive};
    }
    if (std::strcmp(emotion, "laughing") == 0 ||
        std::strcmp(emotion, "funny") == 0 ||
        std::strcmp(emotion, "delicious") == 0) {
        return ExpressionRenderModel{MMAP_EMOJI_NORMAL_HAPPY_EAF, true, 20,
                                     MMAP_EMOJI_NORMAL_ICON_BATTERY_BIN, ExpressionUiMode::kImmersive};
    }
    if (std::strcmp(emotion, "sad") == 0) {
        return ExpressionRenderModel{MMAP_EMOJI_NORMAL_SAD_EAF, true, 20,
                                     MMAP_EMOJI_NORMAL_ICON_BATTERY_BIN, ExpressionUiMode::kImmersive};
    }
    if (std::strcmp(emotion, "crying") == 0) {
        return ExpressionRenderModel{MMAP_EMOJI_NORMAL_CRY_EAF, true, 20,
                                     MMAP_EMOJI_NORMAL_ICON_BATTERY_BIN, ExpressionUiMode::kImmersive};
    }
    if (std::strcmp(emotion, "angry") == 0) {
        return ExpressionRenderModel{MMAP_EMOJI_NORMAL_ANGRY_EAF, true, 20,
                                     MMAP_EMOJI_NORMAL_ICON_BATTERY_BIN, ExpressionUiMode::kImmersive};
    }
    if (std::strcmp(emotion, "surprised") == 0 || std::strcmp(emotion, "shocked") == 0) {
        return ExpressionRenderModel{MMAP_EMOJI_NORMAL_SHOCKED_EAF, true, 20,
                                     MMAP_EMOJI_NORMAL_ICON_BATTERY_BIN, ExpressionUiMode::kImmersive};
    }
    if (std::strcmp(emotion, "thinking") == 0 || std::strcmp(emotion, "embarrassed") == 0) {
        return ExpressionRenderModel{MMAP_EMOJI_NORMAL_CONFUSED_EAF, true, 20,
                                     MMAP_EMOJI_NORMAL_ICON_BATTERY_BIN, ExpressionUiMode::kImmersive};
    }
    if (std::strcmp(emotion, "silly") == 0 || std::strcmp(emotion, "confused") == 0) {
        return ExpressionRenderModel{MMAP_EMOJI_NORMAL_CONFUSED_EAF, true, 20,
                                     MMAP_EMOJI_NORMAL_ICON_BATTERY_BIN, ExpressionUiMode::kImmersive};
    }
    if (std::strcmp(emotion, "sleepy") == 0) {
        return ExpressionRenderModel{MMAP_EMOJI_NORMAL_SLEEP_EAF, true, 16,
                                     MMAP_EMOJI_NORMAL_ICON_BATTERY_BIN, ExpressionUiMode::kImmersive};
    }
    if (std::strcmp(emotion, "relaxed") == 0 ||
        std::strcmp(emotion, "neutral") == 0 ||
        std::strcmp(emotion, "idle") == 0) {
        return ExpressionRenderModel{MMAP_EMOJI_NORMAL_NEUTRAL_EAF, true, 20,
                                     MMAP_EMOJI_NORMAL_ICON_BATTERY_BIN, ExpressionUiMode::kImmersive};
    }

    return std::nullopt;
}

}  // namespace anim
