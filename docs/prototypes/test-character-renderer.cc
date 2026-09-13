#include "character_preview.h"
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <vector>

int main(int argc,char** argv) {
    using namespace anim;
    std::vector<uint8_t> storage(kCharacterBytes+32,0xa5);
    auto* frame=storage.data()+16;
    assert(!RenderCharacterPreview(nullptr,kCharacterBytes,CharacterPreview::kEyes,0));
    assert(!RenderCharacterPreview(frame,1,CharacterPreview::kEyes,0));
    assert(!RenderCharacterPreview(frame,kCharacterBytes,CharacterPreview::kEyes,std::numeric_limits<float>::quiet_NaN()));
    const CharacterPreview scenes[]={CharacterPreview::kEyes,CharacterPreview::kWave,CharacterPreview::kGuitar};
    std::vector<uint8_t> cached_storage(kCharacterBytes+32,0xa5),base(kCharacterBytes);
    auto* cached=cached_storage.data()+16;
    assert(RenderGuitarBase(base.data(),base.size()));
    const auto original_base=base;
    assert(!RenderCachedGuitar(nullptr,kCharacterBytes,base.data(),base.size(),0));
    assert(!RenderCachedGuitar(cached,1,base.data(),base.size(),0));
    assert(!RenderCachedGuitar(cached,kCharacterBytes,nullptr,base.size(),0));
    assert(!RenderCachedGuitar(cached,kCharacterBytes,base.data(),1,0));
    assert(!RenderCachedGuitar(base.data(),base.size(),base.data(),base.size(),0));
    assert(!RenderCachedGuitar(cached,kCharacterBytes,base.data(),base.size(),-1));
    assert(!RenderCachedGuitar(cached,kCharacterBytes,base.data(),base.size(),std::numeric_limits<float>::infinity()));
    for(int i=0;i<540;i++){
        const float seconds=i/30.f;
        assert(RenderCharacterPreview(frame,kCharacterBytes,CharacterPreview::kGuitar,seconds));
        assert(RenderCachedGuitar(cached,kCharacterBytes,base.data(),base.size(),seconds));
        assert(std::memcmp(frame,cached,kCharacterBytes)==0);
    }
    assert(base==original_base);
    for(size_t i=0;i<16;i++){assert(cached_storage[i]==0xa5);assert(cached_storage[kCharacterBytes+16+i]==0xa5);}
    for(int scene=0;scene<3;scene++){
        for(int i=0;i<180;i++)assert(RenderCharacterPreview(frame,kCharacterBytes,scenes[scene],i/30.f));
        assert(RenderCharacterPreview(frame,kCharacterBytes,scenes[scene],1.2f));
        for(size_t i=0;i<16;i++){assert(storage[i]==0xa5);assert(storage[kCharacterBytes+16+i]==0xa5);}
        for(size_t i=360*360*2;i<kCharacterBytes;i++)assert(frame[i]==255);
        // RGB565 of #081312 is 0x0882, stored in panel byte order.
        assert(frame[0]==0x08 && frame[1]==0x82);
        if(argc>1){
            char path[1024];std::snprintf(path,sizeof(path),"%s-%d.ppm",argv[1],scene);
            FILE* out=std::fopen(path,"wb");assert(out);std::fprintf(out,"P6\n360 360\n255\n");
            for(int i=0;i<360*360;i++){
                unsigned c=(frame[2*i]<<8)|frame[2*i+1];
                unsigned char rgb[]={static_cast<unsigned char>(((c>>11)&31)*255/31),static_cast<unsigned char>(((c>>5)&63)*255/63),static_cast<unsigned char>((c&31)*255/31)};
                std::fwrite(rgb,1,3,out);
            }
            std::fclose(out);
        }
    }
    std::puts("PASS: 540 rendered frames; 540 cached guitar frames byte-identical to full redraw; immutable cache, bounds, alpha, byte order and invalid inputs");
}
