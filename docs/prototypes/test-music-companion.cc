#include "character_preview.h"
#include "music_companion_clock.h"
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <vector>

int main() {
    using namespace anim;
    MusicCompanionClock clock;
    clock.Reset(100);
    clock.SetRunning(100, true);
    assert(clock.Seconds(1000100)==1);
    clock.SetRunning(1000100, false);
    assert(clock.Seconds(9000100)==1);
    clock.SetRunning(9000100, true);
    clock.SetRunning(10000100, true); // duplicate playing must not restart
    assert(clock.Seconds(11000100)==3);
    clock.SetRunning(11000100, false); // high-priority overlay freezes
    assert(clock.Seconds(30000100)==3);
    clock.Reset(30000100); // replacement track or fresh session
    assert(clock.Seconds(40000100)==0);
    clock.SetRunning(40000100, true);
    assert(clock.Seconds(40000000)==0); // regressing timestamp
    std::vector<uint8_t> memory(kCharacterBytes+32,0xa5), frozen(kCharacterBytes), eyes(kCharacterBytes);
    auto* frame=memory.data()+16;
    assert(!RenderMusicCompanion(nullptr,kCharacterBytes,0));
    assert(!RenderMusicCompanion(frame,kCharacterBytes-1,0));
    assert(!RenderMusicCompanion(frame,kCharacterBytes,-1));
    assert(!RenderMusicCompanion(frame,kCharacterBytes,std::numeric_limits<double>::infinity()));
    assert(!RenderMusicCompanion(frame,kCharacterBytes,std::numeric_limits<double>::quiet_NaN()));
    assert(!RenderMusicCompanion(frame,kCharacterBytes,0,CharacterPreview::kEyes));
    assert(RenderCharacterPreview(eyes.data(),eyes.size(),CharacterPreview::kEyes,0));
    unsigned frames=0;
    for(auto instrument:{CharacterPreview::kShaker,CharacterPreview::kDrum,CharacterPreview::kKeys,CharacterPreview::kGuitar}) {
    for(double time=0;time<=30;time+=.05) {
        assert(RenderMusicCompanion(frame,kCharacterBytes,time,instrument));
        for(size_t i=kCharacterSize*kCharacterSize*2;i<kCharacterBytes;++i) assert(frame[i]==255);
        if(time>=.7) {
            // The lower screen must still contain props, including beyond the
            // old finite-duration boundary. Not a visual/pixel approval test.
            assert(std::memcmp(frame+220*kCharacterSize*2,eyes.data()+220*kCharacterSize*2,
                               140*kCharacterSize*2)!=0);
        }
        ++frames;
    }
    for(double time:{7.0,14.0,86400.0,31536000.0,1e100}) {
        assert(RenderMusicCompanion(frame,kCharacterBytes,time,instrument));
        assert(RenderMusicCompanion(frozen.data(),frozen.size(),time,instrument));
        assert(std::memcmp(frame,frozen.data(),kCharacterBytes)==0);
        assert(RenderMusicCompanion(frozen.data(),frozen.size(),time+.1,instrument));
        if(time<1e100) assert(std::memcmp(frame,frozen.data(),kCharacterBytes)!=0);
    }
    }
    for(unsigned i=0;i<16;++i) assert(memory[i]==0xa5 && memory[kCharacterBytes+16+i]==0xa5);
    std::printf("PASS: %u companion frames, continuous props, frozen timestamps, long durations, bounds and invalid input\n",frames);
}
