#pragma once
#include "character_preview.h"
#include <string>
namespace anim {
struct MusicCompanionPreferences {
    bool enabled = true; // Preserve this opt-in trial's existing default.
    CharacterPreview instrument = CharacterPreview::kShaker;
    static constexpr CharacterPreview instruments[] = {CharacterPreview::kShaker,
        CharacterPreview::kDrum, CharacterPreview::kKeys, CharacterPreview::kGuitar};
    int Encode() const {
        for (int i=0;i<4;++i) if (instruments[i]==instrument) return i*2+(enabled?1:0);
        return 1;
    }
    static MusicCompanionPreferences Decode(int value) {
        if(value<0 || value>7) return {};
        return {bool(value&1),instruments[value/2]};
    }
    bool Update(const std::string& mode,const std::string& name) {
        auto next=*this;
        if(!mode.empty()) {
            if(mode=="cat") next.enabled=true;
            else if(mode=="cover") next.enabled=false;
            else return false;
        }
        if(!name.empty()) {
            const char* names[]={"shaker","drum","keys","guitar"};
            int index=0;
            while(index<4 && name!=names[index]) ++index;
            if(index==4) return false;
            next.instrument=instruments[index];
            next.enabled=true; // Asking for an instrument also selects the cat.
        }
        *this=next;
        return true;
    }
};
}
