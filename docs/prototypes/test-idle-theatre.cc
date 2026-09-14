#include "idle_theatre.h"
#include <cassert>
#include <cstdio>

using namespace anim;
using Mode = TheatreMode;
using Act = TheatreAct;

int main() {
    for (auto mode : {Mode::kQuiet, Mode::kLively}) {
        const uint64_t base = mode == Mode::kQuiet ? 90000 : 45000;
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
            for (uint64_t t = end+1; t < end+120000; t += 101)
                assert(!s.Tick(t, mode, true).started);
        }
    }
    {
        IdleTheatre s([] { return 0; });
        assert(s.Tick(0, Mode::kOff, true).next_ms == IdleTheatre::kNever);
        assert(s.Tick(0, Mode::kLively, true).next_ms == 45000);
        auto r = s.Tick(45000, Mode::kLively, true);
        assert(r.act == Act::kChin);
        r = s.Tick(46000, Mode::kLively, false);
        assert(r.stopped && r.act == Act::kNone);
        assert(s.Tick(50000, Mode::kLively, false).next_ms == IdleTheatre::kNever);
        r = s.Tick(51000, Mode::kQuiet, true);
        assert(r.next_ms == 166000); // cancelled act still imposes cooldown
        assert(s.Tick(52000, Mode::kQuiet, true).next_ms == 166000);
        r = s.Tick(166000, Mode::kQuiet, true);
        assert(r.started && r.act != Act::kChin);
        r = s.Tick(167000, Mode::kOff, true);
        assert(r.stopped && r.act == Act::kNone);
        r = s.Tick(168000, Mode::kLively, true);
        assert(r.next_ms == 287000); // off/on cannot bypass cooldown
    }
    {
        IdleTheatre s([] { return 0; });
        s.Tick(0, Mode::kQuiet, false);
        auto r = s.Tick(299999, Mode::kQuiet, true);
        assert(r.next_ms == 300000);
        r = s.Tick(300000, Mode::kQuiet, true);
        assert(r.next_ms == 480000 && !r.started);
        assert(s.Tick(300001, Mode::kQuiet, true).next_ms == 480000);
        assert(s.Tick(299000, Mode::kQuiet, true).next_ms == 480000); // clock regression
        r = s.Tick(480000, Mode::kQuiet, true);
        assert(r.started && !r.sleepy);
        s.Tick(485000, Mode::kQuiet, true);
        assert(!s.Tick(599999, Mode::kQuiet, true).sleepy);
        r = s.Tick(600000, Mode::kQuiet, true);
        assert(r.sleepy && r.started && r.act == Act::kRub);
        r = s.Tick(605400, Mode::kQuiet, true);
        assert(r.sleepy && r.stopped && r.act == Act::kNone);
        assert(s.Tick(900000, Mode::kQuiet, true).act == Act::kNone);
        r = s.Tick(900001, Mode::kQuiet, true, true);
        assert(!r.sleepy && r.next_ms == 990001);
    }
    {
        IdleTheatre s([] { return 0; });
        s.Tick(0, Mode::kQuiet, false);
        auto r = s.Tick(700000, Mode::kQuiet, false);
        assert(!r.started && r.act == Act::kNone);
        r = s.Tick(700001, Mode::kQuiet, true);
        assert(r.act == Act::kRub);
        r = s.Tick(700002, Mode::kQuiet, false);
        assert(r.stopped);
        r = s.Tick(700003, Mode::kQuiet, true);
        assert(!r.started && r.act == Act::kNone); // cancelled rub never resumes
    }
    {
        IdleTheatre s([] { return 0; });
        s.Tick(0, Mode::kQuiet, true);
        const auto r = s.Tick(1000000, Mode::kQuiet, true);
        assert(r.act == Act::kRub); // late tick never replays queued play
    }
    {
        IdleTheatre s([] { return 0; });
        s.Tick(0, Mode::kQuiet, false);
        s.Tick(300000, Mode::kQuiet, true);
        assert(s.Tick(599900, Mode::kQuiet, true).started);
        const auto r = s.Tick(600000, Mode::kQuiet, true);
        assert(r.stopped && r.started && r.sleepy && r.act == Act::kRub);
        assert(s.Tick(600001, Mode::kQuiet, false, true).stopped);
        const auto resumed = s.Tick(600002, Mode::kQuiet, true);
        assert(!resumed.sleepy && resumed.act == Act::kNone);
        assert(resumed.next_ms == 720001); // user interrupt retains cooldown
    }
    // Four equiprobable slots: quiet retains chin's 2:1:1 weight; lively
    // includes peek. The last act is removed before random selection.
    unsigned counts[6]{};
    for (uint32_t entropy = 0; entropy < 4; ++entropy) {
        IdleTheatre quiet([entropy] { return entropy; });
        auto due = quiet.Tick(0, Mode::kQuiet, true).next_ms;
        ++counts[static_cast<unsigned>(quiet.Tick(due, Mode::kQuiet, true).act)];
        IdleTheatre lively([entropy] { return entropy; });
        due = lively.Tick(0, Mode::kLively, true).next_ms;
        assert(static_cast<unsigned>(lively.Tick(due, Mode::kLively, true).act) == entropy+1);
    }
    assert(counts[static_cast<unsigned>(Act::kChin)] == 2);
    assert(counts[static_cast<unsigned>(Act::kBubble)] == 1);
    assert(counts[static_cast<unsigned>(Act::kFish)] == 1);
    assert(counts[static_cast<unsigned>(Act::kPeek)] == 0);
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
        const auto mode = (now / 1000000) % 2 ? Mode::kQuiet : Mode::kLively;
        const auto r = s.Tick(now, mode, eligible, interaction);
        assert(eligible || r.act == Act::kNone);
        if (r.stopped) { ended = now; has_ended = true; }
        if (r.started && r.act != Act::kRub) {
            assert(r.act != previous);
            assert(!has_ended || now >= ended+120000);
            assert(mode != Mode::kQuiet || r.act != Act::kPeek);
            previous = r.act;
            ++starts;
        }
    }
    assert(starts > 100);
    std::printf("PASS: interval endpoints, cooldown, no-repeat, interruptions, off/on, slow/sleep boundaries, late/backward clocks and 24h seeded simulation (%u acts)\n", starts);
}
