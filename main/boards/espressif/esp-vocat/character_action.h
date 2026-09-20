#pragma once
#include "character_preview.h"
#include <optional>
#include <cstdint>
namespace anim {
class CharacterActionRequest {
public:
    bool Queue(const char* name, int64_t now) {
        CharacterPreview pose;
        if (!CharacterAction(name,pose)) return false;
        pose_=pose; active_=false; deadline_=now+30000000; return true;
    }
    void Cancel() { pose_.reset(); active_=false; }
    std::optional<CharacterPreview> Tick(int64_t now,bool eligible,bool cancel) {
        completed_.reset();
        if (!cancel && active_ && eligible && now>=deadline_) completed_=pose_;
        if (cancel || (pose_ && now>=deadline_) || (active_ && !eligible)) Cancel();
        if (!pose_ || !eligible) return {};
        if (!active_) { active_=true; deadline_=now+CharacterPreviewDurationMs(*pose_)*1000LL; }
        return pose_;
    }
    int64_t Deadline() const { return pose_?deadline_:INT64_MAX; }
    std::optional<CharacterPreview> Completed() const { return completed_; }
private:
    std::optional<CharacterPreview> pose_;
    std::optional<CharacterPreview> completed_;
    bool active_=false;
    int64_t deadline_=INT64_MAX;
};
}
