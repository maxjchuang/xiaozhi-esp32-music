#include "expression_director.h"
#include "character_semantics.h"
#include <cassert>
#include <cstdio>
#include <vector>
#include <limits>
#include <fstream>
using namespace anim;
int main(int argc,char** argv) {
    std::vector<uint8_t> storage(kCharacterBytes+32,0xa5);
    auto* frame=storage.data()+16;
    unsigned frames=0;
    for(int pose=0;pose<=static_cast<int>(CharacterPreview::kSurprised);++pose) {
        for(int i=0;i<120;i++) {
            assert(RenderCharacterPreview(frame,kCharacterBytes,static_cast<CharacterPreview>(pose),i/10.f));
            for(int j=0;j<16;j++) assert(storage[j]==0xa5 && storage[kCharacterBytes+16+j]==0xa5);
            for(size_t j=360*360*2;j<kCharacterBytes;j++) assert(frame[j]==255);
            ++frames;
        }
        assert(RenderCharacterPreview(frame,kCharacterBytes,static_cast<CharacterPreview>(pose),1e20f));
        if(argc>1) {
            assert(RenderCharacterPreview(frame,kCharacterBytes,static_cast<CharacterPreview>(pose),2));
            std::ofstream out(std::string(argv[1])+"/pose-"+std::to_string(pose)+".ppm",std::ios::binary);
            out<<"P6\n360 360\n255\n";
            for(int p=0;p<360*360;p++) {
                unsigned c=(frame[2*p]<<8)|frame[2*p+1];
                const char rgb[]={static_cast<char>(((c>>11)&31)*255/31),static_cast<char>(((c>>5)&63)*255/63),static_cast<char>((c&31)*255/31)};
                out.write(rgb,3);
            }
            assert(out.good());
        }
    }
    assert(!RenderCharacterPreview(frame,kCharacterBytes,static_cast<CharacterPreview>(99),0));
    assert(!RenderCharacterPreview(frame,kCharacterBytes,CharacterPreview::kListen,std::numeric_limits<float>::infinity()));
    ExpressionRenderModel shown{};
    ExpressionDirector director([&](const auto& model){shown=model;});
    const CharacterPreview expected_states[]={CharacterPreview::kStartup,CharacterPreview::kThink,
        CharacterPreview::kEyes,CharacterPreview::kWave,CharacterPreview::kListen,
        CharacterPreview::kThink,CharacterPreview::kThink,CharacterPreview::kSpeak,
        CharacterPreview::kHappy,CharacterPreview::kSad,CharacterPreview::kSurprised};
    for(int i=0;i<11;i++) {
        director.SetBaseBehavior({static_cast<DisplayBehavior>(i),DisplayBehaviorSource::kDeviceState});
        assert(shown.character_pose==expected_states[i]);
    }
    director.SetBaseBehavior({DisplayBehavior::kIdle,DisplayBehaviorSource::kDeviceState});
    for(const char* emotion:{"happy","laughing","funny","loving","embarrassed","confident","delicious","sad","crying","sleepy","silly","angry","surprised","shocked","thinking","winking","relaxed","confused","neutral","idle"}) {
        director.SetCloudEmotion(emotion);
        assert(shown.character_pose);
        TestAdvance(5001);
    }
    for(const char* name:{"wave","chin","rub","bubble","heart","fish","peek","shaker","drum","keys","guitar"}) {
        CharacterPreview expected; assert(CharacterAction(name,expected));
        director.NotifyUserInteraction();
        assert(director.RequestCharacterAction(name));
        assert(shown.character_pose==expected);
        TestAdvance(CharacterPreviewDurationMs(expected)+1);
        assert(shown.character_pose==(expected==CharacterPreview::kRub?CharacterPreview::kSleepy:CharacterPreview::kEyes));
        assert(director.RequestCharacterAction(name));
        director.NotifyUserInteraction();
        assert(shown.character_pose==CharacterPreview::kEyes);
        director.SetBaseBehavior({DisplayBehavior::kSpeaking,DisplayBehaviorSource::kDeviceState});
        assert(director.RequestCharacterAction(name));
        director.SetBaseBehavior({DisplayBehavior::kListening,DisplayBehaviorSource::kDeviceState});
        assert(shown.character_pose==CharacterPreview::kListen);
        director.SetBaseBehavior({DisplayBehavior::kIdle,DisplayBehaviorSource::kDeviceState});
        assert(shown.character_pose==expected);
        director.SetMediaBehavior({DisplayBehavior::kMusicPlaying,DisplayBehaviorSource::kMusic});
        assert(!director.RequestCharacterAction(name));
        director.ClearMediaBehavior();
        assert(shown.character_pose!=expected || expected==CharacterPreview::kEyes);
    }
    director.NotifyUserInteraction();
    director.SetCloudEmotion("unknown-test");
    assert(shown.character_pose==CharacterPreview::kEyes);
    CharacterActionRequest request;
    assert(request.Queue("heart",0)); assert(!request.Tick(30000000,true,false));
    assert(!request.Queue("invalid",0));
    assert(request.Queue("heart",0)); assert(request.Queue("peek",1));
    assert(request.Tick(2,true,false)==CharacterPreview::kPeek);
    assert(!request.Tick(3,false,false)); assert(!request.Tick(4,true,false));
    std::vector<uint8_t> chin(kCharacterBytes);
    assert(RenderCharacterPreview(frame,kCharacterBytes,CharacterPreview::kThink,4));
    assert(RenderCharacterPreview(chin.data(),chin.size(),CharacterPreview::kChin,1));
    assert(std::equal(chin.begin(),chin.end(),frame));
    assert(RenderCharacterPreview(frame,kCharacterBytes,CharacterPreview::kThink,9));
    assert(!std::equal(chin.begin(),chin.end(),frame));
    std::printf("PASS: %u renderer frames, 11 states, 20 emotions, 11 normal action routes, expiry, replacement, cancellation and music blocking\n",frames);
}
