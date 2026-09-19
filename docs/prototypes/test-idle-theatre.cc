#include "idle_theatre.h"
#include <cassert>
#include <cstdio>

using namespace anim;
using Mode = TheatreMode;
using Act = TheatreAct;

int main() {
    {
        const auto mode = Mode::kEnabled;
        constexpr uint64_t base = 30000;
        for (uint32_t entropy : {0u, static_cast<uint32_t>(base)}) {
            IdleTheatre s([entropy] { return entropy; });
            auto r = s.Tick(0, mode, true);
            assert(r.next_ms == base + entropy);
            const auto due = r.next_ms;
            for (uint64_t t = 1; t < due; t += 113) {
                r = s.Tick(t, mode, true);
                assert(!r.started && r.next_ms == due);
            }
            r = s.Tick(due, mode, true);
            assert(r.started && r.act != Act::kNone);
            const auto first = r.act;
            const auto end = r.next_ms;
            r = s.Tick(end-1, mode, true);
            assert(!r.stopped && r.act == first);
            r = s.Tick(end, mode, true);
            assert(r.stopped && r.act == Act::kNone);
            // next_ms may be the 5-minute policy boundary, not a play deadline.
            for (uint64_t t = end+1; t < end+60000; t += 101)
                assert(!s.Tick(t, mode, true).started);
        }
    }
    {
        IdleTheatre s([] { return 0; });
        assert(s.Tick(0, Mode::kOff, true).next_ms == IdleTheatre::kNever);
        assert(s.Tick(0, Mode::kEnabled, true).next_ms == 30000);
        auto r = s.Tick(45000, Mode::kEnabled, true);
        assert(r.act == Act::kChin);
        r = s.Tick(46000, Mode::kEnabled, false);
        assert(r.stopped && r.act == Act::kNone);
        assert(s.Tick(50000, Mode::kEnabled, false).next_ms == IdleTheatre::kNever);
        r = s.Tick(51000, Mode::kEnabled, true);
        assert(r.next_ms == 106000); // cancelled act still imposes cooldown
        assert(s.Tick(52000, Mode::kEnabled, true).next_ms == 106000);
        r = s.Tick(106000, Mode::kEnabled, true);
        assert(r.started && r.act != Act::kChin);
        r = s.Tick(107000, Mode::kOff, true);
        assert(r.stopped && r.act == Act::kNone);
        r = s.Tick(108000, Mode::kEnabled, true);
        assert(r.next_ms == 167000); // off/on cannot bypass cooldown
    }
    {
        IdleTheatre s([] { return 0; });
        s.Tick(0, Mode::kEnabled, false);
        auto r = s.Tick(299999, Mode::kEnabled, true);
        assert(r.next_ms == 300000);
        r = s.Tick(300000, Mode::kEnabled, true);
        assert(r.next_ms == 360000 && !r.started);
        assert(s.Tick(300001, Mode::kEnabled, true).next_ms == 360000);
        assert(s.Tick(299000, Mode::kEnabled, true).next_ms == 360000); // clock regression
        r = s.Tick(360000, Mode::kEnabled, true);
        assert(r.started && !r.sleepy);
        s.Tick(365000, Mode::kEnabled, true);
        assert(!s.Tick(599999, Mode::kEnabled, true).sleepy);
        r = s.Tick(600000, Mode::kEnabled, true);
        assert(r.sleepy && r.started && r.act == Act::kRub);
        r = s.Tick(605400, Mode::kEnabled, true);
        assert(r.sleepy && r.stopped && r.act == Act::kNone);
        assert(s.Tick(900000, Mode::kEnabled, true).act == Act::kNone);
        r = s.Tick(900001, Mode::kEnabled, true, true);
        assert(!r.sleepy && r.next_ms == 930001);
    }
    {
        IdleTheatre s([] { return 0; });
        s.Tick(0, Mode::kEnabled, false);
        auto r = s.Tick(700000, Mode::kEnabled, false);
        assert(!r.started && r.act == Act::kNone);
        r = s.Tick(700001, Mode::kEnabled, true);
        assert(r.act == Act::kRub);
        r = s.Tick(700002, Mode::kEnabled, false);
        assert(r.stopped);
        r = s.Tick(700003, Mode::kEnabled, true);
        assert(!r.started && r.act == Act::kNone); // cancelled rub never resumes
    }
    {
        IdleTheatre s([] { return 0; });
        s.Tick(0, Mode::kEnabled, true);
        const auto r = s.Tick(1000000, Mode::kEnabled, true);
        assert(r.act == Act::kRub); // late tick never replays queued play
    }
    {
        IdleTheatre s([] { return 0; });
        s.Tick(0, Mode::kEnabled, false);
        s.Tick(300000, Mode::kEnabled, true);
        assert(s.Tick(599900, Mode::kEnabled, true).started);
        const auto r = s.Tick(600000, Mode::kEnabled, true);
        assert(r.stopped && r.started && r.sleepy && r.act == Act::kRub);
        assert(s.Tick(600001, Mode::kEnabled, false, true).stopped);
        const auto resumed = s.Tick(600002, Mode::kEnabled, true);
        assert(!resumed.sleepy && resumed.act == Act::kNone);
        assert(resumed.next_ms == 660001); // user interrupt retains cooldown
    }
    // Six slots retain chin's double weight and include every quiet act.
    // The last act is removed before random selection.
    unsigned counts[7]{};
    for (uint32_t entropy = 0; entropy < 6; ++entropy) {
        IdleTheatre quiet([entropy] { return entropy; });
        auto due = quiet.Tick(0, Mode::kEnabled, true).next_ms;
        ++counts[static_cast<unsigned>(quiet.Tick(due, Mode::kEnabled, true).act)];
    }
    assert(counts[static_cast<unsigned>(Act::kChin)] == 2);
    assert(counts[static_cast<unsigned>(Act::kBubble)] == 1);
    assert(counts[static_cast<unsigned>(Act::kFish)] == 1);
    assert(counts[static_cast<unsigned>(Act::kPeek)] == 1);
    assert(counts[static_cast<unsigned>(Act::kHeart)] == 1);
    // Seeded stress: real interactions reset inactivity, but not history or
    // cooldown. Alternate mode/busy states and check invariants for 24h.
    uint32_t seed = 41;
    const auto random = [&] { seed = seed*1664525u+1013904223u; return seed; };
    IdleTheatre s(random);
    uint64_t ended = 0;
    bool has_ended = false;
    Act previous = Act::kNone;
    unsigned starts = 0;
    for (uint64_t now = 0; now < 86400000; now += 100) {
        const bool eligible = (now % 400000) < 390000;
        const bool interaction = now % 250000 == 0;
        const auto mode = (now / 1000000) % 2 ? Mode::kEnabled : Mode::kOff;
        const auto r = s.Tick(now, mode, eligible, interaction);
        assert(eligible || r.act == Act::kNone);
        if (r.stopped) { ended = now; has_ended = true; }
        if (r.started && r.act != Act::kRub) {
            assert(r.act != previous);
            assert(!has_ended || now >= ended+60000);
            assert(mode == Mode::kEnabled);
            previous = r.act;
            ++starts;
        }
    }
    assert(starts > 100);
    std::printf("PASS: interval endpoints, cooldown, no-repeat, interruptions, off/on, slow/sleep boundaries, late/backward clocks and 24h seeded simulation (%u acts)\n", starts);
}
