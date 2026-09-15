// Compile the real director with virtual ESP timers and no device dependencies.
#include "expression_director.h"
#include <cassert>
#include <cstdio>
using namespace anim;
static_assert(TheatreDurationMs(TheatreAct::kChin) == CharacterPreviewDurationMs(CharacterPreview::kChin));
static_assert(TheatreDurationMs(TheatreAct::kBubble) == CharacterPreviewDurationMs(CharacterPreview::kBubble));
static_assert(TheatreDurationMs(TheatreAct::kFish) == CharacterPreviewDurationMs(CharacterPreview::kFish));
static_assert(TheatreDurationMs(TheatreAct::kPeek) == CharacterPreviewDurationMs(CharacterPreview::kPeek));
static_assert(TheatreDurationMs(TheatreAct::kRub) == CharacterPreviewDurationMs(CharacterPreview::kRub));
int main() {
    ExpressionRenderModel shown{};
    {
        ExpressionDirector director([&](const auto& model) { shown = model; });
        auto idle = [&] { director.SetBaseBehavior({DisplayBehavior::kIdle, DisplayBehaviorSource::kDeviceState}); };
        idle();
        assert(shown.character_pose == CharacterPreview::kEyes);
#if CONFIG_ECHOEAR_IDLE_THEATRE_TRIAL
        constexpr int wait = 30000;
        TestAdvance(wait-1);
        idle(); // Repeated status must not postpone the first act.
        assert(shown.character_pose == CharacterPreview::kEyes);
        TestAdvance(1);
        assert(shown.character_pose == CharacterPreview::kChin);
        TestAdvance(5000);
        assert(shown.character_pose == CharacterPreview::kEyes);
        TestAdvance(59999);
        assert(shown.character_pose == CharacterPreview::kEyes);
        TestAdvance(1);
        assert(shown.character_pose == CharacterPreview::kBubble);
        director.SetMediaBehavior({DisplayBehavior::kMusicPlaying, DisplayBehaviorSource::kMusic});
        assert(!shown.character_pose && shown.music_scene_visible);
        assert(shown.music_animation_running);
        TestAdvance(10000);
        director.ClearMediaBehavior();
        director.NotifyUserInteraction();
        assert(shown.character_pose == CharacterPreview::kEyes);
        TestAdvance(120000);
        assert(shown.character_pose.has_value());
        director.SetTheatreBlocked(true);
        assert(shown.character_pose == CharacterPreview::kEyes);
        TestAdvance(200000);
        assert(shown.character_pose == CharacterPreview::kEyes);
        director.SetTheatreBlocked(false);
        director.NotifyUserInteraction();
        TestAdvance(wait);
        assert(shown.character_pose && shown.character_pose != CharacterPreview::kEyes);
        director.PostTransientBehavior({DisplayBehavior::kWakeAcknowledged, DisplayBehaviorSource::kWakeWord, {}, 900});
        assert(shown.character_pose == CharacterPreview::kWave);
        director.SetBaseBehavior({DisplayBehavior::kListening, DisplayBehaviorSource::kDeviceState});
        TestAdvance(901);
        assert(!shown.character_pose);
        idle();
        director.NotifyUserInteraction();
        TestAdvance(600000);
        assert(shown.character_pose == CharacterPreview::kRub);
        TestAdvance(5400);
        assert(!shown.character_pose); // Legacy sleepy scene, not another act.
        TestAdvance(600000);
        assert(!shown.character_pose);
        director.NotifyUserInteraction();
        assert(shown.character_pose == CharacterPreview::kEyes);
        TestAdvance(wait);
        assert(shown.character_pose && shown.character_pose != CharacterPreview::kEyes);
        director.PostTransientBehavior({DisplayBehavior::kFatalError, DisplayBehaviorSource::kSystem, {}, 1000});
        assert(!shown.character_pose);
        TestAdvance(1001);
        director.NotifyUserInteraction();
        for (auto media : {DisplayBehavior::kMusicBuffering, DisplayBehavior::kMusicPaused}) {
            director.SetMediaBehavior({media, DisplayBehaviorSource::kMusic});
            TestAdvance(200000);
            assert(!shown.character_pose && shown.music_scene_visible);
            assert(!shown.music_animation_running);
            director.ClearMediaBehavior();
            director.NotifyUserInteraction();
        }
        director.SetCloudEmotion("happy");
        assert(!shown.character_pose);
        TestAdvance(5001);
        assert(shown.character_pose == CharacterPreview::kEyes);
        director.PostTransientBehavior({DisplayBehavior::kRecoverableError, DisplayBehaviorSource::kSystem, "charge", 4500});
        assert(!shown.character_pose && shown.text == "charge");
        TestAdvance(4501);
        assert(shown.character_pose == CharacterPreview::kEyes);
#else
        TestAdvance(300000);
        assert(!shown.character_pose); // Legacy five-minute sleep is unchanged.
#endif
    }
    test_timer_fail = true;
    {
        ExpressionDirector director([&](const auto& model) { shown = model; });
        director.SetBaseBehavior({DisplayBehavior::kIdle, DisplayBehaviorSource::kDeviceState});
        TestAdvance(700000);
        assert(shown.character_pose == CharacterPreview::kEyes);
    }
    std::puts("PASS: actual director deadlines, priority, interruption, self-test blocking, sleepy recovery and timer failure");
}
