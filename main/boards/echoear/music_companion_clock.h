#pragma once
#include <algorithm>
#include <cstdint>

namespace anim {
// Called under the display's lifetime mutex. A new track resets the clock;
// pauses and higher-priority overlays preserve its accumulated animation age.
class MusicCompanionClock {
public:
    void Reset(int64_t now) { elapsed_ = 0; anchor_ = now; running_ = false; }
    void SetRunning(int64_t now, bool running) {
        now = std::max(now, anchor_);
        if (running_) elapsed_ += now-anchor_;
        anchor_ = now;
        running_ = running;
    }
    double Seconds(int64_t now) const {
        return (elapsed_ + (running_ ? std::max<int64_t>(0, now-anchor_) : 0))/1000000.0;
    }
private:
    int64_t elapsed_ = 0, anchor_ = 0;
    bool running_ = false;
};
}
