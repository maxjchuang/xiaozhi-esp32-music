#ifndef MUSIC_H
#define MUSIC_H

#include <string>

struct MusicPlaybackRequest {
    std::string audio_url;
    std::string song_name;
    std::string metadata_url;
};

class Music {
public:
    virtual ~Music() = default;  // 添加虚析构函数
    
    virtual bool Download(const std::string& song_name, const std::string& artist_name = "") = 0;
    virtual std::string GetDownloadResult() = 0;
    virtual bool PlayUrl(const std::string& music_url, const std::string& song_name) = 0;
    virtual bool Play(const MusicPlaybackRequest& request) {
        return PlayUrl(request.audio_url, request.song_name);
    }
    
    // 新增流式播放相关方法
    virtual bool StartStreaming(const std::string& music_url) = 0;
    virtual bool StopStreaming() = 0;  // 停止流式播放
    // Signal-only stop for latency-sensitive control paths. Implementations
    // must return without joining worker threads or allocating another task.
    virtual bool RequestStopStreaming() = 0;
    virtual size_t GetBufferSize() const = 0;
    virtual bool IsPlaying() const = 0;
    virtual bool IsDownloading() const = 0;
    virtual int16_t* GetAudioData() = 0;
};

#endif // MUSIC_H
