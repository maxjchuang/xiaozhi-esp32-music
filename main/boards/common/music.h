#ifndef MUSIC_H
#define MUSIC_H

#include <cstddef>
#include <string>

struct MusicPlaybackRequest {
    std::string audio_url;
    std::string song_name;
    std::string metadata_url;
};

class Music {
public:
    virtual ~Music() = default;
    virtual bool Play(const MusicPlaybackRequest& request) = 0;
    virtual bool RequestStop() = 0;
    virtual bool IsPlaying() const = 0;
    virtual void RecordSessionTelemetry(const std::string& event_type,
                                        const std::string& value = "") = 0;
};

#endif
