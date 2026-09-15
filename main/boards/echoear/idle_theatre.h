#pragma once

#include <algorithm>
#include <cstdint>
#include <functional>
#include <limits>
#include <utility>

namespace anim {

enum class TheatreMode { kOff, kEnabled };
enum class TheatreAct { kNone, kChin, kBubble, kFish, kPeek, kRub };

constexpr uint32_t TheatreDurationMs(TheatreAct act) {
    switch (act) {
    case TheatreAct::kChin: return 5000;
    case TheatreAct::kBubble: return 7000;
    case TheatreAct::kFish: return 6500;
    case TheatreAct::kPeek: return 6000;
    case TheatreAct::kRub: return 5400;
    default: return 0;
    }
}

// Pure scheduling policy. Caller supplies monotonic milliseconds and entropy.
// Only real user input sets interaction=true; repeated device states do not.
// No rendering, allocation per tick, timers, hardware sleep or cloud calls.
class IdleTheatre {
public:
    static constexpr uint64_t kNever = std::numeric_limits<uint64_t>::max();
    struct Output {
        TheatreAct act;
        bool started;
        bool stopped;
        bool sleepy;
        uint64_t next_ms;
    };

    explicit IdleTheatre(std::function<uint32_t()> random) : random_(std::move(random)) {}

    Output Tick(uint64_t now, TheatreMode mode, bool eligible, bool interaction = false) {
        // A regressing caller clock must not rewind cooldown or replay acts.
        now = std::max(now, last_tick_);
        last_tick_ = now;
        if (!initialized_) { initialized_ = true; last_user_ = now; }
        bool stopped = false, started = false;
        const auto stop = [&]() {
            if (active_ != TheatreAct::kNone) {
                active_ = TheatreAct::kNone;
                cooldown_until_ = Add(now, 60000);
                stopped = true;
            }
            next_ = kNever;
        };
        if (interaction) {
            stop();
            last_user_ = now;
            rub_shown_ = false;
            slow_ = false;
        }
        if (mode != mode_ || eligible != eligible_) stop();
        mode_ = mode;
        eligible_ = eligible;
        const uint64_t inactive = now - last_user_;
        const bool newly_slow = !slow_ && inactive >= 300000;
        slow_ = inactive >= 300000;
        if (mode == TheatreMode::kOff || !eligible) {
            stop();
            return {active_, false, stopped, mode != TheatreMode::kOff && inactive >= 600000, kNever};
        }

        if (active_ != TheatreAct::kNone && now >= ends_) stop();
        if (inactive >= 600000) {
            // Sleep boundary preempts play. Rub at most once, even if cancelled.
            if (active_ != TheatreAct::kNone && active_ != TheatreAct::kRub) stop();
            if (!rub_shown_) {
                active_ = TheatreAct::kRub;
                ends_ = Add(now, TheatreDurationMs(active_));
                rub_shown_ = true;
                started = true;
            }
            return {active_, started, stopped, true, active_ == TheatreAct::kNone ? kNever : ends_};
        }

        // Slow-down affects pending waits too, but is applied only once.
        if (newly_slow && active_ == TheatreAct::kNone) next_ = kNever;
        if (active_ == TheatreAct::kNone) {
            if (next_ == kNever) {
                constexpr uint64_t base = 30000;
                const uint64_t delay = (base + Random() % (base + 1)) * (slow_ ? 2 : 1);
                next_ = std::max(Add(now, delay), cooldown_until_);
            } else if (now >= next_) {
                active_ = Pick();
                previous_ = active_;
                ends_ = Add(now, TheatreDurationMs(active_));
                next_ = kNever;
                started = true;
            }
        }
        uint64_t deadline = active_ == TheatreAct::kNone ? next_ : ends_;
        if (!slow_) deadline = std::min(deadline, Add(last_user_, 300000));
        deadline = std::min(deadline, Add(last_user_, 600000));
        return {active_, started, stopped, false, deadline};
    }

private:
    static uint64_t Add(uint64_t a, uint64_t b) { return a > kNever-b ? kNever : a+b; }
    uint32_t Random() { return random_ ? random_() : 0; }
    TheatreAct Pick() {
        const TheatreAct pool[] = {TheatreAct::kChin, TheatreAct::kChin, TheatreAct::kBubble, TheatreAct::kFish};
        TheatreAct candidates[4]; unsigned count = 0;
        for (unsigned i = 0; i < 4; ++i) if (pool[i] != previous_) candidates[count++] = pool[i];
        return candidates[Random() % count];
    }

    std::function<uint32_t()> random_;
    TheatreMode mode_ = TheatreMode::kOff;
    TheatreAct active_ = TheatreAct::kNone, previous_ = TheatreAct::kNone;
    bool initialized_ = false, eligible_ = false, slow_ = false, rub_shown_ = false;
    uint64_t last_tick_ = 0, last_user_ = 0, next_ = kNever, ends_ = 0, cooldown_until_ = 0;
};

} // namespace anim
