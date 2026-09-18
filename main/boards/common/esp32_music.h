#ifndef ESP32_MUSIC_H
#define ESP32_MUSIC_H

#include "music.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

class Esp32Music final : public Music {
public:
    Esp32Music();
    ~Esp32Music() override;

    bool Play(const MusicPlaybackRequest& request) override;
    bool RequestStop() override;
    bool IsPlaying() const override { return playing_.load(); }
    void RecordSessionTelemetry(const std::string& event_type,
                                const std::string& value = "") override;

private:
    struct SessionEvent {
        std::string session_id;
        std::string type;
        std::string value;
        int64_t monotonic_ms = 0;
        uint32_t sequence = 0;
    };

    static constexpr size_t kMaxBufferedBytes = 256 * 1024;
    static constexpr size_t kStartBufferedBytes = 32 * 1024;

    void DownloadTask(uint32_t generation, std::string url);
    void PlaybackTask(uint32_t generation);
    void StopAndJoin(bool terminal_event);
    void ClearBuffer();
    bool RecreateDecoder();
    void ResetTelemetry(uint32_t generation, const std::string& metadata_url);
    void MarkTelemetryStarted(uint32_t generation);
    void FinalizeTelemetry(uint32_t generation, const char* event_type, const char* end_reason);
    std::vector<int16_t> ConvertToNativeMono(const int16_t* pcm, int sample_count, int channels,
                                             int source_rate) const;

    std::atomic<bool> playing_{false};
    std::atomic<bool> downloading_{false};
    std::atomic<uint32_t> generation_{0};
    std::atomic<int64_t> played_ms_{0};
    std::thread download_thread_;
    std::thread playback_thread_;
    std::deque<std::vector<uint8_t>> chunks_;
    size_t buffered_bytes_ = 0;
    mutable std::mutex buffer_mutex_;
    std::condition_variable buffer_cv_;
    void* decoder_ = nullptr;

    std::string song_name_;
    std::mutex telemetry_mutex_;
    std::string telemetry_url_;
    std::string playback_id_;
    std::string playback_session_id_;
    std::string session_id_;
    std::string boot_nonce_;
    std::vector<SessionEvent> pending_events_;
    std::atomic<uint32_t> session_sequence_{0};
    std::atomic<int64_t> started_ms_{-1};
    std::atomic<uint32_t> underrun_count_{0};
    std::atomic<int64_t> underrun_ms_{0};
    std::atomic<bool> telemetry_finalized_{true};
};

#endif
