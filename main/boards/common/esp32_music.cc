#include "esp32_music.h"

#include "application.h"
#include "audio/audio_codec.h"
#include "board.h"
#include "display.h"
#include "display/lvgl_display/jpg/jpeg_to_image.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <sstream>

#include <esp_app_desc.h>
#include <esp_audio_dec_default.h>
#include <esp_audio_simple_dec.h>
#include <esp_audio_simple_dec_default.h>
#include <esp_heap_caps.h>
#include <esp_jpeg_common.h>
#include <esp_log.h>
#include <esp_pthread.h>
#include <esp_random.h>
#include <esp_timer.h>
#include <cJSON.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#define TAG "Esp32Music"

namespace {

void ConfigureThread(const char* name, size_t stack_size, int priority, bool external = true) {
    auto config = esp_pthread_get_default_config();
    config.stack_size = stack_size;
    config.prio = priority;
    config.thread_name = name;
    config.stack_alloc_caps =
        external ? (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) : (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (auto error = esp_pthread_set_cfg(&config); error != ESP_OK) {
        ESP_LOGW(TAG, "Unable to configure %s: %s", name, esp_err_to_name(error));
    }
}

std::string TelemetryUrl(const std::string& manifest_url) {
    constexpr std::string_view suffix = "/manifest.json";
    if (manifest_url.size() <= suffix.size() ||
        manifest_url.compare(manifest_url.size() - suffix.size(), suffix.size(), suffix) != 0) {
        return {};
    }
    return manifest_url.substr(0, manifest_url.size() - suffix.size()) + "/telemetry";
}

void PostJson(std::string url, std::string body) {
    auto network = Board::GetInstance().GetNetwork();
    auto http = network ? network->CreateHttp(0) : nullptr;
    if (!http) {
        ESP_LOGW(TAG, "Telemetry HTTP client unavailable");
        return;
    }
    http->SetTimeout(10000);
    http->SetHeader("Content-Type", "application/json");
    http->SetHeader("Accept", "application/json");
    http->SetContent(std::move(body));
    auto opened = http->Open("POST", url);
    if (!opened) {
        ESP_LOGW(TAG, "Telemetry request failed: %s", opened.error().ToString().c_str());
        return;
    }
    auto status = http->GetStatusCode();
    http->Close();
    if (!status || *status < 200 || *status >= 300) {
        ESP_LOGW(TAG, "Telemetry rejected");
    } else {
        ESP_LOGI(TAG, "MUSIC_TELEMETRY uploaded status=%d", *status);
    }
}

size_t Id3TagSize(const std::vector<uint8_t>& data) {
    if (data.size() < 10 || std::memcmp(data.data(), "ID3", 3) != 0) {
        return 0;
    }
    return 10 + ((data[6] & 0x7f) << 21) + ((data[7] & 0x7f) << 14) + ((data[8] & 0x7f) << 7) +
           (data[9] & 0x7f);
}

}  // namespace

Esp32Music::Esp32Music() {
    char nonce[17];
    snprintf(nonce, sizeof(nonce), "%08lx%08lx", static_cast<unsigned long>(esp_random()),
             static_cast<unsigned long>(esp_random()));
    boot_nonce_ = nonce;
    esp_audio_dec_register_default();
    esp_audio_simple_dec_register_default();
    RecreateDecoder();
}

bool Esp32Music::RecreateDecoder() {
    if (decoder_) {
        esp_audio_simple_dec_close(decoder_);
        decoder_ = nullptr;
    }
    esp_audio_simple_dec_cfg_t config = {
        .dec_type = ESP_AUDIO_SIMPLE_DEC_TYPE_MP3,
        .dec_cfg = nullptr,
        .cfg_size = 0,
        .use_frame_dec = false,
    };
    if (esp_audio_simple_dec_open(&config, &decoder_) != ESP_AUDIO_ERR_OK) {
        decoder_ = nullptr;
        ESP_LOGE(TAG, "Failed to initialize ESP MP3 decoder");
        return false;
    }
    return true;
}

Esp32Music::~Esp32Music() {
    StopAndJoin(false);
    if (decoder_) {
        esp_audio_simple_dec_close(decoder_);
    }
}

bool Esp32Music::Play(const MusicPlaybackRequest& request) {
    if (request.audio_url.empty()) {
        return false;
    }
    StopAndJoin(IsPlaying());
    ClearBuffer();
    // Reopening is slightly more expensive than reset, but isolates every
    // stream from decoder state and avoids the ESP codec reset path corrupting
    // adjacent audio-effect state after repeated play/stop cycles.
    if (!RecreateDecoder()) {
        return false;
    }
    song_name_ = request.song_name.empty() ? "在线音乐" : request.song_name;
    played_ms_ = 0;
    current_lyric_index_ = -1;
    track_duration_ms_ = 0;
    lyric_offset_ms_ = 0;
    last_progress_update_ms_ = -1;
    {
        std::lock_guard<std::mutex> lock(lyrics_mutex_);
        lyrics_.clear();
    }
    underrun_count_ = 0;
    underrun_ms_ = 0;
    const uint32_t generation = ++generation_;
    ResetTelemetry(generation, request.metadata_url);
    playing_ = true;
    downloading_ = true;
    if (auto* display = Board::GetInstance().GetDisplay()) {
        display->SetMusicArtwork(nullptr, 0, 0, nullptr, 0, 0);
        display->SetMusicTrackInfo({song_name_, "", "", 0});
        display->SetMusicLyricWindow("", request.metadata_url.empty() ? "暂无歌词" : "歌词加载中…",
                                     "");
    }

    ConfigureThread("music_download", 8192, 3);
    download_thread_ = std::thread(&Esp32Music::DownloadTask, this, generation, request.audio_url);
    ConfigureThread("music_playback", 12288, 4);
    playback_thread_ = std::thread(&Esp32Music::PlaybackTask, this, generation);
    if (!request.metadata_url.empty()) {
        ConfigureThread("music_metadata", 12288, 3);
        metadata_thread_ =
            std::thread(&Esp32Music::MetadataTask, this, generation, request.metadata_url);
    }
    ESP_LOGI(TAG, "MUSIC_PLAY requested generation=%lu song=%s",
             static_cast<unsigned long>(generation), song_name_.c_str());
    return true;
}

bool Esp32Music::RequestStop() {
    const uint32_t stopped_generation = generation_.load();
    const bool was_playing = playing_.exchange(false);
    const bool was_downloading = downloading_.exchange(false);
    const bool active = was_playing || was_downloading;
    if (active) {
        ++generation_;
        buffer_cv_.notify_all();
        Application::GetInstance().Schedule([]() {
            if (auto* display = Board::GetInstance().GetDisplay()) {
                display->SetMusicPlaybackActive(false);
            }
        });
        FinalizeTelemetry(stopped_generation, "playback_stopped", "user_stopped");
    }
    return active;
}

void Esp32Music::StopAndJoin(bool terminal_event) {
    const uint32_t old_generation = generation_.load();
    const bool active = RequestStop();
    if (download_thread_.joinable()) {
        download_thread_.join();
    }
    if (playback_thread_.joinable()) {
        playback_thread_.join();
    }
    if (metadata_thread_.joinable()) {
        metadata_thread_.join();
    }
    if (terminal_event && active) {
        FinalizeTelemetry(old_generation, "playback_stopped", "user_stopped");
    }
    ClearBuffer();
}

void Esp32Music::MetadataTask(uint32_t generation, std::string metadata_url) {
    // The play tool first speaks a confirmation over the same Wi-Fi/audio path.
    // Wait until that session has closed and the MP3 has a healthy lead so the
    // optional artwork request cannot delay either TTS or playback startup.
    while (generation == generation_ && playing_) {
        bool audio_ready = false;
        {
            std::lock_guard<std::mutex> lock(buffer_mutex_);
            audio_ready = buffered_bytes_ >= kMaxBufferedBytes / 2 || !downloading_;
        }
        if (audio_ready && Application::GetInstance().GetDeviceState() == kDeviceStateIdle) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    if (generation != generation_ || !playing_) {
        return;
    }
    constexpr std::string_view kManifestSuffix = "manifest.json";
    if (metadata_url.size() <= kManifestSuffix.size() ||
        metadata_url.compare(metadata_url.size() - kManifestSuffix.size(), kManifestSuffix.size(),
                             kManifestSuffix) != 0) {
        return;
    }
    const std::string resource_base =
        metadata_url.substr(0, metadata_url.size() - kManifestSuffix.size());
    auto download = [this, generation](const std::string& url, const char* accept, size_t limit,
                                       std::vector<uint8_t>& body) {
        auto network = Board::GetInstance().GetNetwork();
        auto http = network ? network->CreateHttp(0) : nullptr;
        if (!http) {
            return false;
        }
        http->SetTimeout(15000);
        http->SetHeader("Accept", accept);
        auto opened = http->Open("GET", url);
        if (!opened) {
            ESP_LOGW(TAG, "Music metadata resource unavailable: %s",
                     opened.error().ToString().c_str());
            return false;
        }
        auto status = http->GetStatusCode();
        const size_t declared = http->GetBodyLength();
        if (!status || *status != 200 || declared > limit) {
            http->Close();
            return false;
        }
        body.clear();
        body.reserve(declared > 0 ? declared : std::min<size_t>(limit, 64 * 1024));
        std::array<char, 4096> chunk;
        while (generation == generation_ && playing_) {
            auto count = http->Read(chunk.data(), chunk.size());
            if (!count || *count == 0) {
                break;
            }
            if (body.size() + *count > limit) {
                body.clear();
                break;
            }
            body.insert(body.end(), reinterpret_cast<uint8_t*>(chunk.data()),
                        reinterpret_cast<uint8_t*>(chunk.data()) + *count);
        }
        http->Close();
        return !body.empty() && (declared == 0 || body.size() == declared) &&
               generation == generation_ && playing_;
    };

    std::vector<uint8_t> manifest;
    if (!download(metadata_url, "application/json", 64 * 1024, manifest)) {
        return;
    }
    manifest.push_back('\0');
    std::unique_ptr<cJSON, decltype(&cJSON_Delete)> root(
        cJSON_Parse(reinterpret_cast<const char*>(manifest.data())), cJSON_Delete);
    auto* schema = root ? cJSON_GetObjectItem(root.get(), "schema_version") : nullptr;
    if (!root || !cJSON_IsNumber(schema) || schema->valueint != 1) {
        ESP_LOGW(TAG, "Unsupported music manifest");
        return;
    }
    auto json_string = [&root](const char* key) -> std::string {
        auto* item = cJSON_GetObjectItem(root.get(), key);
        return cJSON_IsString(item) && item->valuestring ? item->valuestring : "";
    };
    MusicTrackInfo track{json_string("title"), json_string("artist"), json_string("album"), 0};
    auto* duration = cJSON_GetObjectItem(root.get(), "duration_ms");
    if (cJSON_IsNumber(duration)) {
        track.duration_ms = std::max(0, duration->valueint);
    }
    if (track.title.empty()) {
        track.title = song_name_;
    }
    track_duration_ms_ = track.duration_ms;
    if (auto* display = Board::GetInstance().GetDisplay()) {
        display->SetMusicTrackInfo(track);
    }

    bool lyrics_loaded = false;
    auto* lyrics = cJSON_GetObjectItem(root.get(), "lyrics");
    if (cJSON_IsObject(lyrics)) {
        auto* offset = cJSON_GetObjectItem(lyrics, "offset_ms");
        if (cJSON_IsNumber(offset) && offset->valueint >= -5000 && offset->valueint <= 5000) {
            lyric_offset_ms_ = offset->valueint;
        }
        std::vector<uint8_t> lrc;
        if (download(resource_base + "lyrics.lrc", "text/plain", 256 * 1024, lrc)) {
            lyrics_loaded =
                ParseLyrics(std::string(reinterpret_cast<char*>(lrc.data()), lrc.size()));
        }
    }
    if (auto* display = Board::GetInstance().GetDisplay()) {
        display->SetMusicLyricWindow("", lyrics_loaded ? "" : "暂无歌词", "");
    }
    ESP_LOGI(TAG, "MUSIC_LYRICS ready=%d lines=%u", lyrics_loaded,
             static_cast<unsigned>(lyrics_.size()));

    std::vector<uint8_t> background_jpeg;
    std::vector<uint8_t> disc_jpeg;
    if (!download(resource_base + "background.jpg", "image/jpeg", 512 * 1024, background_jpeg) ||
        !download(resource_base + "disc.jpg", "image/jpeg", 512 * 1024, disc_jpeg)) {
        ESP_LOGW(TAG, "Music artwork download failed");
        return;
    }
    auto decode = [](const std::vector<uint8_t>& jpeg, int expected_width, int expected_height,
                     uint8_t** pixels) {
        size_t output_size = 0;
        size_t width = 0;
        size_t height = 0;
        size_t stride = 0;
        return jpeg_to_image(jpeg.data(), jpeg.size(), pixels, &output_size, &width, &height,
                             &stride) == ESP_OK &&
               width == static_cast<size_t>(expected_width) &&
               height == static_cast<size_t>(expected_height) &&
               stride == static_cast<size_t>(expected_width * 2) &&
               output_size >= static_cast<size_t>(expected_width * expected_height * 2);
    };
    uint8_t* background = nullptr;
    uint8_t* disc = nullptr;
    const bool decoded =
        decode(background_jpeg, 360, 360, &background) && decode(disc_jpeg, 192, 192, &disc);
    if (decoded && generation == generation_ && playing_) {
        if (auto* display = Board::GetInstance().GetDisplay()) {
            display->SetMusicArtwork(reinterpret_cast<uint16_t*>(background), 360, 360,
                                     reinterpret_cast<uint16_t*>(disc), 192, 192);
        }
    } else {
        ESP_LOGW(TAG, "Music artwork decode failed");
    }
    if (background) {
        jpeg_free_align(background);
    }
    if (disc) {
        jpeg_free_align(disc);
    }
}

bool Esp32Music::ParseLyrics(const std::string& content) {
    std::vector<std::pair<int, std::string>> parsed;
    std::istringstream stream(content);
    std::string line;
    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        const size_t close = line.find(']');
        const size_t colon = line.find(':');
        if (line.empty() || line[0] != '[' || close == std::string::npos ||
            colon == std::string::npos || colon > close) {
            continue;
        }
        const std::string minutes_text = line.substr(1, colon - 1);
        if (minutes_text.empty() ||
            !std::all_of(minutes_text.begin(), minutes_text.end(),
                         [](unsigned char ch) { return std::isdigit(ch); })) {
            continue;
        }
        int minutes = 0;
        bool valid_minutes = true;
        for (char ch : minutes_text) {
            const int digit = ch - '0';
            if (minutes > (std::numeric_limits<int>::max() - digit) / 10) {
                valid_minutes = false;
                break;
            }
            minutes = minutes * 10 + digit;
        }
        const std::string seconds_text = line.substr(colon + 1, close - colon - 1);
        char* seconds_end = nullptr;
        const double seconds = std::strtod(seconds_text.c_str(), &seconds_end);
        if (!valid_minutes || seconds_text.empty() || seconds_end == seconds_text.c_str() ||
            *seconds_end != '\0' || seconds < 0 || seconds >= 60 ||
            minutes > std::numeric_limits<int>::max() / 60000) {
            continue;
        }
        parsed.emplace_back(minutes * 60000 + static_cast<int>(seconds * 1000),
                            line.substr(close + 1));
    }
    std::sort(parsed.begin(), parsed.end());
    std::lock_guard<std::mutex> lock(lyrics_mutex_);
    lyrics_ = std::move(parsed);
    current_lyric_index_ = -1;
    return !lyrics_.empty();
}

void Esp32Music::UpdateMusicDisplay(int64_t position_ms) {
    auto* display = Board::GetInstance().GetDisplay();
    if (!display) {
        return;
    }
    const int64_t last_progress = last_progress_update_ms_.load();
    if (last_progress < 0 || position_ms - last_progress >= 500) {
        last_progress_update_ms_ = position_ms;
        display->UpdateMusicProgress(static_cast<int>(position_ms), track_duration_ms_);
    }
    std::string previous;
    std::string current;
    std::string next;
    int new_index = -1;
    {
        std::lock_guard<std::mutex> lock(lyrics_mutex_);
        const int64_t lyric_time = position_ms + lyric_offset_ms_.load();
        for (size_t index = 0; index < lyrics_.size() && lyrics_[index].first <= lyric_time;
             ++index) {
            new_index = static_cast<int>(index);
        }
        if (new_index == current_lyric_index_) {
            return;
        }
        current_lyric_index_ = new_index;
        if (new_index >= 0) {
            current = lyrics_[new_index].second;
            if (new_index > 0) {
                previous = lyrics_[new_index - 1].second;
            }
            if (new_index + 1 < static_cast<int>(lyrics_.size())) {
                next = lyrics_[new_index + 1].second;
            }
        }
    }
    display->SetMusicLyricWindow(previous, current, next);
}

void Esp32Music::ClearBuffer() {
    std::lock_guard<std::mutex> lock(buffer_mutex_);
    chunks_.clear();
    buffered_bytes_ = 0;
}

void Esp32Music::DownloadTask(uint32_t generation, std::string url) {
    // The play tool is called while the assistant is still speaking its
    // acknowledgement. Starting a large HTTP transfer at that point can starve
    // the MQTT TTS stream on this board and make the acknowledgement stutter.
    const int64_t acknowledgement_wait_started_us = esp_timer_get_time();
    bool waited_for_acknowledgement = false;
    while (downloading_ && playing_ && generation == generation_ &&
           Application::GetInstance().GetDeviceState() == kDeviceStateSpeaking) {
        waited_for_acknowledgement = true;
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    if (waited_for_acknowledgement) {
        ESP_LOGI(TAG, "MUSIC_DOWNLOAD deferred_for_tts_ms=%lld",
                 static_cast<long long>((esp_timer_get_time() - acknowledgement_wait_started_us) /
                                        1000));
    }
    if (!downloading_ || !playing_ || generation != generation_) {
        buffer_cv_.notify_all();
        return;
    }

    size_t offset = 0;
    bool complete = false;
    constexpr int kAttempts = 4;
    for (int attempt = 1;
         attempt <= kAttempts && downloading_ && playing_ && generation == generation_ && !complete;
         ++attempt) {
        auto http = Board::GetInstance().GetNetwork()->CreateHttp(0);
        if (!http) {
            break;
        }
        http->SetTimeout(15000);
        http->SetHeader("Accept", "audio/mpeg, */*");
        http->SetHeader("Accept-Encoding", "identity");
        http->SetHeader("Range", "bytes=" + std::to_string(offset) + "-");
        auto opened = http->Open("GET", url);
        if (!opened) {
            ESP_LOGW(TAG, "Music connect %d/%d failed: %s", attempt, kAttempts,
                     opened.error().ToString().c_str());
            vTaskDelay(pdMS_TO_TICKS(300));
            continue;
        }
        auto status = http->GetStatusCode();
        const bool accepted = status && ((offset == 0 && (*status == 200 || *status == 206)) ||
                                         (offset > 0 && *status == 206));
        if (!accepted) {
            ESP_LOGE(TAG, "Music HTTP range rejected at %u", static_cast<unsigned>(offset));
            http->Close();
            break;
        }
        const size_t declared = http->GetBodyLength();
        size_t received = 0;
        bool read_failed = false;
        std::array<char, 4096> buffer;
        while (downloading_ && playing_ && generation == generation_) {
            auto count = http->Read(buffer.data(), buffer.size());
            if (!count) {
                ESP_LOGW(TAG, "Music read failed: %s", count.error().ToString().c_str());
                read_failed = true;
                break;
            }
            if (*count == 0) {
                complete = declared == 0 || received >= declared;
                read_failed = !complete;
                break;
            }
            std::vector<uint8_t> chunk(reinterpret_cast<uint8_t*>(buffer.data()),
                                       reinterpret_cast<uint8_t*>(buffer.data()) + *count);
            std::unique_lock<std::mutex> lock(buffer_mutex_);
            buffer_cv_.wait(lock, [this, generation]() {
                return buffered_bytes_ < kMaxBufferedBytes || !downloading_ || !playing_ ||
                       generation != generation_;
            });
            if (!downloading_ || !playing_ || generation != generation_) {
                break;
            }
            buffered_bytes_ += chunk.size();
            offset += chunk.size();
            received += chunk.size();
            chunks_.push_back(std::move(chunk));
            lock.unlock();
            buffer_cv_.notify_all();
        }
        http->Close();
        if (read_failed && attempt < kAttempts) {
            vTaskDelay(pdMS_TO_TICKS(300));
        }
    }
    if (generation == generation_) {
        downloading_ = false;
    }
    buffer_cv_.notify_all();
    if (!complete && playing_ && generation == generation_) {
        ESP_LOGE(TAG, "Music stream ended before completion at %u bytes",
                 static_cast<unsigned>(offset));
    }
}

std::vector<int16_t> Esp32Music::ConvertToNativeMono(const int16_t* pcm, int sample_count,
                                                     int channels, int source_rate) const {
    if (!pcm || sample_count <= 0 || channels <= 0 || source_rate <= 0) {
        return {};
    }
    const int mono_count = sample_count / channels;
    std::vector<int16_t> mono(mono_count);
    for (int i = 0; i < mono_count; ++i) {
        int32_t sum = 0;
        for (int channel = 0; channel < channels; ++channel) {
            sum += pcm[i * channels + channel];
        }
        mono[i] = static_cast<int16_t>(sum / channels);
    }
    auto codec = Board::GetInstance().GetAudioCodec();
    const int target_rate = codec ? codec->output_sample_rate() : source_rate;
    if (target_rate == source_rate || mono.size() < 2) {
        return mono;
    }
    const size_t output_count = std::max<size_t>(
        1, (mono.size() * static_cast<size_t>(target_rate) + source_rate / 2) / source_rate);
    std::vector<int16_t> output(output_count);
    const uint64_t step =
        output_count > 1 ? ((static_cast<uint64_t>(mono.size() - 1) << 16) / (output_count - 1))
                         : 0;
    uint64_t position = 0;
    for (size_t i = 0; i < output_count; ++i) {
        const size_t index = std::min<size_t>(position >> 16, mono.size() - 1);
        const size_t next = std::min(index + 1, mono.size() - 1);
        const int fraction = position & 0xffff;
        output[i] = static_cast<int16_t>(
            mono[index] + ((static_cast<int32_t>(mono[next]) - mono[index]) * fraction >> 16));
        position += step;
    }
    return output;
}

void Esp32Music::PlaybackTask(uint32_t generation) {
    {
        std::unique_lock<std::mutex> lock(buffer_mutex_);
        buffer_cv_.wait(lock, [this, generation]() {
            return buffered_bytes_ >= kStartBufferedBytes || !downloading_ || !playing_ ||
                   generation != generation_;
        });
    }
    if (!playing_ || generation != generation_) {
        return;
    }

    std::vector<uint8_t> encoded;
    encoded.reserve(64 * 1024);
    bool id3_checked = false;
    int decoded_frames = 0;
    bool decode_failed = false;
    bool scene_visible = false;
    auto& app = Application::GetInstance();

    // The play tool runs inside an active conversation. Let its spoken
    // acknowledgement finish, then close that listening session so music can
    // take over the playback queue. Later wake-ups are real interruptions and
    // only pause music until the conversation returns to idle.
    bool requested_idle = false;
    while (playing_ && generation == generation_) {
        const auto state = app.GetDeviceState();
        if (state == kDeviceStateIdle) {
            break;
        }
        if (state == kDeviceStateListening && !requested_idle) {
            ESP_LOGI(TAG, "Closing listening session before music playback");
            app.ToggleChatState();
            requested_idle = true;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    while (playing_ && generation == generation_) {
        if (app.GetDeviceState() != kDeviceStateIdle) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        {
            std::unique_lock<std::mutex> lock(buffer_mutex_);
            if (chunks_.empty()) {
                if (!downloading_) {
                    break;
                }
                const int64_t before = esp_timer_get_time();
                buffer_cv_.wait(lock, [this, generation]() {
                    return !chunks_.empty() || !downloading_ || !playing_ ||
                           generation != generation_;
                });
                const int64_t waited = (esp_timer_get_time() - before) / 1000;
                if (waited >= 20) {
                    ++underrun_count_;
                    underrun_ms_ += waited;
                }
            }
            if (!chunks_.empty()) {
                auto chunk = std::move(chunks_.front());
                chunks_.pop_front();
                buffered_bytes_ -= chunk.size();
                encoded.insert(encoded.end(), chunk.begin(), chunk.end());
                buffer_cv_.notify_all();
            }
        }

        if (!id3_checked && encoded.size() >= 10) {
            const size_t tag_size = Id3TagSize(encoded);
            if (tag_size > 0 && encoded.size() < tag_size) {
                if (tag_size > 1024 * 1024) {
                    decode_failed = true;
                    break;
                }
                continue;
            }
            if (tag_size > 0) {
                encoded.erase(encoded.begin(), encoded.begin() + tag_size);
            }
            id3_checked = true;
        }

        size_t consumed = 0;
        while (id3_checked && encoded.size() > consumed && playing_ && generation == generation_ &&
               app.GetDeviceState() == kDeviceStateIdle) {
            esp_audio_simple_dec_raw_t raw = {
                .buffer = encoded.data() + consumed,
                .len = static_cast<uint32_t>(encoded.size() - consumed),
                .eos = !downloading_,
                .consumed = 0,
                .frame_recover = ESP_AUDIO_SIMPLE_DEC_RECOVERY_NONE,
            };
            std::vector<uint8_t> pcm_bytes(4608);
            esp_audio_simple_dec_out_t frame = {
                .buffer = pcm_bytes.data(),
                .len = static_cast<uint32_t>(pcm_bytes.size()),
                .needed_size = 0,
                .decoded_size = 0,
            };
            auto result = esp_audio_simple_dec_process(decoder_, &raw, &frame);
            if (result == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH && frame.needed_size > 0) {
                pcm_bytes.resize(frame.needed_size);
                frame.buffer = pcm_bytes.data();
                frame.len = pcm_bytes.size();
                result = esp_audio_simple_dec_process(decoder_, &raw, &frame);
            }
            if (raw.consumed == 0 && frame.decoded_size == 0) {
                break;
            }
            consumed += raw.consumed;
            if (result != ESP_AUDIO_ERR_OK) {
                ESP_LOGW(TAG, "MP3 decode failed: %d", result);
                decode_failed = true;
                break;
            }
            if (frame.decoded_size == 0) {
                continue;
            }
            esp_audio_simple_dec_info_t info{};
            if (esp_audio_simple_dec_get_info(decoder_, &info) != ESP_AUDIO_ERR_OK ||
                info.sample_rate == 0 || info.channel == 0 || info.bits_per_sample != 16) {
                decode_failed = true;
                break;
            }
            const int sample_count = frame.decoded_size / sizeof(int16_t);
            auto output = ConvertToNativeMono(reinterpret_cast<int16_t*>(pcm_bytes.data()),
                                              sample_count, info.channel, info.sample_rate);
            const int frame_ms = (sample_count * 1000) / (info.sample_rate * info.channel);
            played_ms_ += frame_ms;
            UpdateMusicDisplay(played_ms_.load());
            if (!app.GetAudioService().PushPcmToPlaybackQueue(
                    std::move(output), static_cast<uint32_t>(played_ms_.load()), true)) {
                if (generation == generation_ && playing_) {
                    ESP_LOGI(TAG, "Music PCM queue reset by another audio session");
                }
                break;
            }
            MarkTelemetryStarted(generation);
            if (!scene_visible) {
                scene_visible = true;
                const std::string title = song_name_;
                app.Schedule([title]() {
                    auto* display = Board::GetInstance().GetDisplay();
                    if (display &&
                        Application::GetInstance().GetDeviceState() == kDeviceStateIdle) {
                        display->SetMusicPlaybackActive(true);
                        display->SetChatMessage("assistant", ("《" + title + "》").c_str());
                        // This label describes local playback and has no matching
                        // TTS audio. Apply the status last so Vocat can clear the
                        // legacy speaker icon raised by SetChatMessage.
                        display->SetStatus("音乐播放中");
                        display->SetEmotion("happy");
                    }
                });
            }
            ++decoded_frames;
        }
        if (consumed > 0 && consumed <= encoded.size()) {
            encoded.erase(encoded.begin(), encoded.begin() + consumed);
        }
        if (encoded.size() > 512 * 1024) {
            decode_failed = true;
            break;
        }
    }

    if (generation != generation_) {
        return;
    }
    playing_ = false;
    downloading_ = false;
    buffer_cv_.notify_all();
    if (decode_failed || decoded_frames == 0) {
        FinalizeTelemetry(generation, "playback_failed", "decode_failed");
    } else {
        FinalizeTelemetry(generation, "playback_completed", "natural_completed");
    }
    ESP_LOGI(TAG, "MUSIC_PLAY finished frames=%d played_ms=%lld", decoded_frames,
             static_cast<long long>(played_ms_.load()));
    app.Schedule([]() {
        auto* display = Board::GetInstance().GetDisplay();
        if (display && Application::GetInstance().GetDeviceState() == kDeviceStateIdle) {
            display->SetMusicPlaybackActive(false);
            display->SetStatus("待命");
            display->SetChatMessage("system", "");
            display->SetEmotion("neutral");
        }
    });
}

void Esp32Music::ResetTelemetry(uint32_t generation, const std::string& metadata_url) {
    std::lock_guard<std::mutex> lock(telemetry_mutex_);
    telemetry_url_ = TelemetryUrl(metadata_url);
    playback_id_ =
        Board::GetInstance().GetUuid() + ":" + boot_nonce_ + ":" + std::to_string(generation);
    playback_session_id_ = session_id_;
    started_ms_ = -1;
    telemetry_finalized_ = false;
}

void Esp32Music::MarkTelemetryStarted(uint32_t generation) {
    if (generation == generation_ && started_ms_ < 0) {
        started_ms_ = esp_timer_get_time() / 1000;
        ESP_LOGI(TAG, "MUSIC_TELEMETRY started playback_id=%s", playback_id_.c_str());
    }
}

void Esp32Music::RecordSessionTelemetry(const std::string& event_type, const std::string& value) {
    static constexpr const char* allowed[] = {"wake_detected", "listening_started",
                                              "listening_stopped", "user_utterance",
                                              "assistant_response"};
    if (std::find(std::begin(allowed), std::end(allowed), event_type) == std::end(allowed)) {
        return;
    }
    std::lock_guard<std::mutex> lock(telemetry_mutex_);
    const int64_t now = esp_timer_get_time() / 1000;
    if (event_type == "wake_detected" || session_id_.empty()) {
        session_id_ =
            Board::GetInstance().GetUuid() + ":session:" + boot_nonce_ + ":" + std::to_string(now);
        session_sequence_ = 0;
    }
    if (pending_events_.size() >= 56) {
        pending_events_.erase(pending_events_.begin());
    }
    pending_events_.push_back(
        {session_id_, event_type, value.substr(0, 1000), now, ++session_sequence_});
}

void Esp32Music::FinalizeTelemetry(uint32_t generation, const char* event_type,
                                   const char* end_reason) {
    if (telemetry_finalized_.exchange(true)) {
        return;
    }
    std::string url;
    std::string playback_id;
    std::string session_id;
    std::vector<SessionEvent> pending;
    {
        std::lock_guard<std::mutex> lock(telemetry_mutex_);
        url = telemetry_url_;
        playback_id = playback_id_;
        session_id = playback_session_id_;
        pending.swap(pending_events_);
    }
    if (url.empty()) {
        ESP_LOGD(TAG, "No telemetry endpoint for generation %lu",
                 static_cast<unsigned long>(generation));
        return;
    }
    std::unique_ptr<cJSON, decltype(&cJSON_Delete)> root(cJSON_CreateObject(), cJSON_Delete);
    cJSON_AddNumberToObject(root.get(), "schema_version", 1);
    cJSON_AddStringToObject(root.get(), "device_id", Board::GetInstance().GetUuid().c_str());
    if (!session_id.empty()) {
        cJSON_AddStringToObject(root.get(), "session_id", session_id.c_str());
    }
    auto* events = cJSON_AddArrayToObject(root.get(), "events");
    for (const auto& item : pending) {
        auto* event = cJSON_CreateObject();
        const std::string id = item.session_id + ":" + std::to_string(item.sequence);
        cJSON_AddStringToObject(event, "event_id", id.c_str());
        cJSON_AddStringToObject(event, "event_type", item.type.c_str());
        cJSON_AddStringToObject(event, "session_id", item.session_id.c_str());
        cJSON_AddNumberToObject(event, "sequence", item.sequence);
        cJSON_AddNumberToObject(event, "monotonic_ms", item.monotonic_ms);
        auto* payload = cJSON_AddObjectToObject(event, "payload");
        if (item.type == "wake_detected") {
            cJSON_AddStringToObject(payload, "wake_method", item.value.c_str());
            cJSON_AddStringToObject(payload, "firmware_version",
                                    esp_app_get_description()->version);
        } else if (item.type == "user_utterance") {
            cJSON_AddStringToObject(payload, "user_text", item.value.c_str());
        } else if (item.type == "assistant_response") {
            cJSON_AddStringToObject(payload, "assistant_text", item.value.c_str());
        }
        cJSON_AddItemToArray(events, event);
    }
    uint32_t sequence = 0;
    auto add_playback_event = [&](const char* type, const char* suffix, bool terminal) {
        auto* event = cJSON_CreateObject();
        const std::string id = playback_id + ":" + suffix;
        cJSON_AddStringToObject(event, "event_id", id.c_str());
        cJSON_AddStringToObject(event, "event_type", type);
        cJSON_AddStringToObject(event, "playback_id", playback_id.c_str());
        if (!session_id.empty())
            cJSON_AddStringToObject(event, "session_id", session_id.c_str());
        cJSON_AddNumberToObject(event, "sequence", ++sequence);
        cJSON_AddNumberToObject(event, "monotonic_ms", esp_timer_get_time() / 1000);
        auto* payload = cJSON_AddObjectToObject(event, "payload");
        cJSON_AddNumberToObject(payload, "audible_played_ms", terminal ? played_ms_.load() : 0);
        if (terminal) {
            cJSON_AddNumberToObject(payload, "underrun_count", underrun_count_.load());
            cJSON_AddNumberToObject(payload, "underrun_total_ms", underrun_ms_.load());
            cJSON_AddStringToObject(payload, "end_reason", end_reason);
        }
        cJSON_AddItemToArray(events, event);
    };
    if (started_ms_ >= 0)
        add_playback_event("playback_started", "started", false);
    add_playback_event(event_type, "ended", true);
    char* json = cJSON_PrintUnformatted(root.get());
    if (!json)
        return;
    std::string body(json);
    cJSON_free(json);
    ConfigureThread("music_telemetry", 6144, 2);
    std::thread(PostJson, std::move(url), std::move(body)).detach();
}
