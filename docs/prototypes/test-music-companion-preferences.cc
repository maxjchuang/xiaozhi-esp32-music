#include "music_companion_preferences.h"
#include <cassert>
#include <cstdio>
int main() {
    using namespace anim;
    for(int i=0;i<8;++i) assert(MusicCompanionPreferences::Decode(i).Encode()==i);
    assert(MusicCompanionPreferences::Decode(-1).Encode()==1);
    assert(MusicCompanionPreferences::Decode(999).Encode()==1);
    MusicCompanionPreferences p;
    for(auto name:{"shaker","drum","keys","guitar"}) {
        assert(p.Update("",name) && p.enabled);
        const auto instrument=p.instrument;
        assert(p.Update("cover","") && !p.enabled && p.instrument==instrument);
        p=MusicCompanionPreferences::Decode(p.Encode());
        assert(!p.enabled && p.instrument==instrument);
        assert(p.Update("cat","") && p.enabled && p.instrument==instrument);
    }
    const auto original=p.Encode();
    assert(!p.Update("invalid","") && p.Encode()==original);
    assert(!p.Update("cover","invalid") && p.Encode()==original);
    std::puts("PASS: all persisted selections, independent view/instrument, invalid data, and round trips");
}
