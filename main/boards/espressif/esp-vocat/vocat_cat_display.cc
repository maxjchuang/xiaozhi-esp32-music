#include "vocat_cat_display.h"

#include "assets/lang_config.h"
#include "music_companion_preferences.h"
#include "settings.h"

#include <algorithm>
#include <cstring>

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <expression_emote.h>

namespace emote {
namespace {

constexpr int kFramePeriodMs = 100;
constexpr char kSettingsNamespace[] = "cat_display";
constexpr char kCompanionKey[] = "companion";
const char* TAG = "VocatCatDisplay";

anim::CharacterPreview SceneForEmotion(const char* emotion) {
    if (!emotion) {
        return anim::CharacterPreview::kEyes;
    }
    if (std::strcmp(emotion, "happy") == 0 || std::strcmp(emotion, "laughing") == 0) {
        return anim::CharacterPreview::kHappy;
    }
    if (std::strcmp(emotion, "confused") == 0) {
        return anim::CharacterPreview::kConfused;
    }
    if (std::strcmp(emotion, "thinking") == 0) {
        return anim::CharacterPreview::kThink;
    }
    if (std::strcmp(emotion, "sleepy") == 0) {
        return anim::CharacterPreview::kSleepy;
    }
    if (std::strcmp(emotion, "sad") == 0 || std::strcmp(emotion, "crying") == 0) {
        return anim::CharacterPreview::kSad;
    }
    if (std::strcmp(emotion, "angry") == 0) {
        return anim::CharacterPreview::kAngry;
    }
    if (std::strcmp(emotion, "surprised") == 0 || std::strcmp(emotion, "shocked") == 0) {
        return anim::CharacterPreview::kSurprised;
    }
    return anim::CharacterPreview::kEyes;
}

}  // namespace

VocatCatDisplay::VocatCatDisplay(esp_lcd_panel_handle_t panel, esp_lcd_panel_io_handle_t panel_io,
                                 int width, int height)
    : EmoteDisplay(panel, panel_io, width, height) {
    state_mutex_ = xSemaphoreCreateMutex();
    Settings settings(kSettingsNamespace);
    companion_preference_ =
        anim::MusicCompanionPreferences::Decode(settings.GetInt(kCompanionKey, 1)).Encode();
}

VocatCatDisplay::~VocatCatDisplay() {
    StopRenderer();
    if (state_mutex_) {
        vSemaphoreDelete(state_mutex_);
        state_mutex_ = nullptr;
    }
    for (auto*& buffer : frame_buffers_) {
        heap_caps_free(buffer);
        buffer = nullptr;
    }
    heap_caps_free(cover_buffer_);
    cover_buffer_ = nullptr;
}

void VocatCatDisplay::LoadAssets() {
    if (render_task_ || !GetEmoteHandle()) {
        return;
    }

    // Create the full-screen character before loading the standard Emote layout.
    // The later-created toast label then stays above the character and retains the
    // complete font loaded from the assets partition.
    character_image_ =
        emote_create_obj_by_type(GetEmoteHandle(), EMOTE_OBJ_TYPE_IMAGE, "vocat_cat_character");
    if (!character_image_) {
        ESP_LOGE(TAG, "Unable to create cat image layer");
        EmoteDisplay::LoadAssets();
        return;
    }
    emote_lock(GetEmoteHandle());
    gfx_obj_set_size(character_image_, anim::kCharacterSize, anim::kCharacterSize);
    gfx_obj_align(character_image_, GFX_ALIGN_CENTER, 0, 0);
    gfx_obj_set_visible(character_image_, false);
    emote_unlock(GetEmoteHandle());

    EmoteDisplay::LoadAssets();
    if (!state_mutex_) {
        return;
    }

    // The cat renderer owns all full-screen state and emotion visuals. Keep the
    // legacy eye/listen animations disabled; their opaque backgrounds otherwise
    // flash over the cat or leave a mismatched rectangle while listening.
    emote_set_obj_visible(GetEmoteHandle(), EMT_DEF_ELEM_EYE_ANIM, false);
    emote_set_obj_visible(GetEmoteHandle(), EMT_DEF_ELEM_LISTEN_ANIM, false);
    emote_set_obj_visible(GetEmoteHandle(), EMT_DEF_ELEM_EMERG_DLG, false);

    for (auto*& buffer : frame_buffers_) {
        buffer = static_cast<uint8_t*>(
            heap_caps_malloc(anim::kCharacterBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (!buffer) {
            ESP_LOGE(TAG, "Unable to allocate cat frame buffers");
            StopRenderer();
            return;
        }
    }
    for (int i = 0; i < 2; ++i) {
        auto& descriptor = frame_descriptors_[i];
        descriptor.header.magic = C_ARRAY_HEADER_MAGIC;
        descriptor.header.cf = GFX_COLOR_FORMAT_RGB565A8;
        descriptor.header.w = anim::kCharacterSize;
        descriptor.header.h = anim::kCharacterSize;
        descriptor.header.stride = anim::kCharacterSize * 2;
        descriptor.data_size = anim::kCharacterBytes;
        descriptor.data = frame_buffers_[i];
    }

    stopping_ = false;
    // Painter::Fill keeps scanline coverage and edge tables on its stack. Match the
    // validated EchoEar renderer's internal 16 KiB stack; an 8 KiB external stack
    // corrupts return addresses during complex speaking/listening frames.
    if (xTaskCreatePinnedToCore(RenderTaskEntry, "vocat_cat", 16 * 1024, this, 1, &render_task_,
                                0) != pdPASS) {
        render_task_ = nullptr;
        ESP_LOGE(TAG, "Unable to start cat renderer");
    }
}

void VocatCatDisplay::SetStatus(const char* status) {
    ESP_LOGI(TAG, "SetStatus: %s", status ? status : "");
    anim::CharacterPreview scene = anim::CharacterPreview::kThink;
    bool idle = false;
    if (status && std::strcmp(status, Lang::Strings::LISTENING) == 0) {
        scene = anim::CharacterPreview::kListen;
        ClearSubtitle();
    } else if (status && std::strcmp(status, Lang::Strings::SPEAKING) == 0) {
        scene = anim::CharacterPreview::kSpeak;
    } else if (status && std::strcmp(status, Lang::Strings::STANDBY) == 0) {
        scene = CurrentBaseScene();
        idle = true;
        ClearSubtitle();
    } else if (status && std::strcmp(status, Lang::Strings::ERROR) == 0) {
        scene = anim::CharacterPreview::kConfused;
    }
    idle_ = idle;
    status_scene_ = static_cast<int>(scene);
    const bool new_interaction = status && std::strcmp(status, Lang::Strings::CONNECTING) == 0;
    const bool error = status && std::strcmp(status, Lang::Strings::ERROR) == 0;
    if ((new_interaction || error) && state_mutex_ &&
        xSemaphoreTake(state_mutex_, pdMS_TO_TICKS(20)) == pdTRUE) {
        action_request_.Cancel();
        xSemaphoreGive(state_mutex_);
    }
}

void VocatCatDisplay::ClearSubtitle() {
    auto handle = GetEmoteHandle();
    if (!handle) {
        return;
    }
    auto* label = emote_get_obj_by_name(handle, EMT_DEF_ELEM_TOAST_LABEL);
    if (!label) {
        return;
    }
    emote_lock(handle);
    gfx_label_set_text(label, "");
    emote_notify_all_refresh(handle);
    emote_unlock(handle);
}

void VocatCatDisplay::SetEmotion(const char* emotion) {
    ESP_LOGI(TAG, "SetEmotion: %s", emotion ? emotion : "");
    const auto scene = SceneForEmotion(emotion);
    emotion_scene_ = static_cast<int>(scene);
    if (idle_) {
        status_scene_ = static_cast<int>(scene);
    }
}

void VocatCatDisplay::SetChatMessage(const char* role, const char* content) {
    EmoteDisplay::SetChatMessage(role, content);
    ESP_LOGI(TAG, "CAT_TEXT role=%s bytes=%u", role ? role : "",
             static_cast<unsigned>(content ? std::strlen(content) : 0));
    if (role && std::strcmp(role, "user") == 0 && state_mutex_ &&
        xSemaphoreTake(state_mutex_, pdMS_TO_TICKS(20)) == pdTRUE) {
        action_request_.Cancel();
        xSemaphoreGive(state_mutex_);
    }
}

bool VocatCatDisplay::RequestCharacterAction(const std::string& name) {
    if (!state_mutex_ || xSemaphoreTake(state_mutex_, pdMS_TO_TICKS(50)) != pdTRUE) {
        return false;
    }
    const bool accepted =
        !music_active_ && action_request_.Queue(name.c_str(), esp_timer_get_time());
    xSemaphoreGive(state_mutex_);
    ESP_LOGI(TAG, "CAT_ACTION request=%s accepted=%d", name.c_str(), accepted);
    return accepted;
}

bool VocatCatDisplay::ConfigureMusicCompanion(const std::string& mode,
                                              const std::string& instrument) {
    auto preferences = anim::MusicCompanionPreferences::Decode(companion_preference_);
    if (!preferences.Update(mode, instrument)) {
        return false;
    }
    Settings settings(kSettingsNamespace, true);
    settings.SetInt(kCompanionKey, preferences.Encode());
    companion_preference_ = preferences.Encode();
    ESP_LOGI(TAG, "CAT_COMPANION mode=%s instrument=%s encoded=%d", mode.c_str(),
             instrument.c_str(), preferences.Encode());
    return true;
}

void VocatCatDisplay::SetMusicPlaybackActive(bool active) {
    if (music_active_.exchange(active) == active) {
        return;
    }
    music_started_us_ = active ? esp_timer_get_time() : 0;
    if (state_mutex_ && xSemaphoreTake(state_mutex_, pdMS_TO_TICKS(20)) == pdTRUE) {
        action_request_.Cancel();
        xSemaphoreGive(state_mutex_);
    }
    ESP_LOGI(TAG, "CAT_COMPANION active=%d", active);
}

void VocatCatDisplay::SetMusicCoverArtwork(const uint16_t* pixels, int width, int height) {
    cover_ready_ = false;
    if (!pixels) {
        return;
    }
    if (width != anim::kCharacterSize || height != anim::kCharacterSize) {
        ESP_LOGW(TAG, "Ignoring music cover with unexpected size %dx%d", width, height);
        return;
    }
    if (!cover_buffer_) {
        cover_buffer_ = static_cast<uint8_t*>(
            heap_caps_malloc(anim::kCharacterBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (!cover_buffer_) {
            ESP_LOGE(TAG, "Unable to allocate music cover buffer");
            return;
        }
        cover_descriptor_.header.magic = C_ARRAY_HEADER_MAGIC;
        cover_descriptor_.header.cf = GFX_COLOR_FORMAT_RGB565A8;
        cover_descriptor_.header.w = anim::kCharacterSize;
        cover_descriptor_.header.h = anim::kCharacterSize;
        cover_descriptor_.header.stride = anim::kCharacterSize * 2;
        cover_descriptor_.data_size = anim::kCharacterBytes;
        cover_descriptor_.data = cover_buffer_;
    }
    constexpr size_t kRgbBytes = anim::kCharacterSize * anim::kCharacterSize * sizeof(uint16_t);
    const auto* source = reinterpret_cast<const uint8_t*>(pixels);
    for (size_t offset = 0; offset < kRgbBytes; offset += sizeof(uint16_t)) {
        // The JPEG decoder returns native little-endian RGB565. Emote's ST77916
        // path swaps image bytes, matching the character renderer's byte order.
        cover_buffer_[offset] = source[offset + 1];
        cover_buffer_[offset + 1] = source[offset];
    }
    std::memset(cover_buffer_ + kRgbBytes, 255, anim::kCharacterSize * anim::kCharacterSize);
    cover_ready_ = true;
    ESP_LOGI(TAG, "MUSIC_COVER ready=1");
}

anim::CharacterPreview VocatCatDisplay::CurrentBaseScene() const {
    return static_cast<anim::CharacterPreview>(emotion_scene_.load());
}

void VocatCatDisplay::RenderTaskEntry(void* context) {
    static_cast<VocatCatDisplay*>(context)->RenderTask();
}

void VocatCatDisplay::RenderTask() {
    int back = 0;
    unsigned frame_count = 0;
    std::optional<anim::CharacterPreview> active_action;
    while (!stopping_) {
        const int64_t now = esp_timer_get_time();
        const auto preferences =
            anim::MusicCompanionPreferences::Decode(companion_preference_.load());
        anim::CharacterPreview scene = static_cast<anim::CharacterPreview>(status_scene_.load());
        const bool music_active = music_active_.load();
        const bool companion = music_active && preferences.enabled;
        const bool cover = music_active && !preferences.enabled && cover_ready_.load();

        if (!companion && state_mutex_ &&
            xSemaphoreTake(state_mutex_, pdMS_TO_TICKS(10)) == pdTRUE) {
            if (auto action = action_request_.Tick(now, true, false)) {
                scene = *action;
                if (!active_action || *active_action != *action) {
                    ESP_LOGI(TAG, "CAT_ACTION started pose=%d", static_cast<int>(*action));
                }
                active_action = action;
            } else if (active_action) {
                ESP_LOGI(TAG, "CAT_ACTION finished pose=%d", static_cast<int>(*active_action));
                active_action.reset();
            }
            xSemaphoreGive(state_mutex_);
        }

        const double seconds = companion
                                   ? static_cast<double>(now - music_started_us_.load()) / 1000000.0
                                   : static_cast<double>(now) / 1000000.0;
        const bool rendered =
            cover ||
            (companion ? anim::RenderMusicCompanion(frame_buffers_[back], anim::kCharacterBytes,
                                                    seconds, preferences.instrument)
                       : anim::RenderCharacterPreview(frame_buffers_[back], anim::kCharacterBytes,
                                                      scene, static_cast<float>(seconds)));
        if (rendered && GetEmoteHandle() && character_image_) {
            emote_lock(GetEmoteHandle());
            gfx_img_set_src(character_image_,
                            cover ? &cover_descriptor_ : &frame_descriptors_[back]);
            gfx_obj_set_visible(character_image_, true);
            emote_set_anim_visible(GetEmoteHandle(), false);
            emote_notify_all_refresh(GetEmoteHandle());
            emote_unlock(GetEmoteHandle());
            if (!cover) {
                back ^= 1;
            }
            if (++frame_count == 100) {
                ESP_LOGI(TAG, "CAT_RENDER stable frames=%u stack_free=%u", frame_count,
                         static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
            }
        }
        vTaskDelay(pdMS_TO_TICKS(kFramePeriodMs));
    }
    render_task_ = nullptr;
    vTaskDelete(nullptr);
}

void VocatCatDisplay::StopRenderer() {
    stopping_ = true;
    for (int attempt = 0; render_task_ && attempt < 100; ++attempt) {
        vTaskDelay(pdMS_TO_TICKS(2));
    }
    if (render_task_) {
        vTaskDelete(render_task_);
        render_task_ = nullptr;
    }
}

}  // namespace emote
