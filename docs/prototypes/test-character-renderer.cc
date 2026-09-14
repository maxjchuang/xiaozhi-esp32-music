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
    assert(!RenderCharacterPreview(frame,kCharacterBytes,static_cast<CharacterPreview>(99),0));
    const CharacterPreview scenes[]={CharacterPreview::kEyes,CharacterPreview::kWave,CharacterPreview::kGuitar,CharacterPreview::kBubble,CharacterPreview::kFish,
        CharacterPreview::kChin,CharacterPreview::kRub,CharacterPreview::kHeart,CharacterPreview::kPeek,CharacterPreview::kShaker,CharacterPreview::kDrum,CharacterPreview::kKeys};
    const auto all=CharacterTestScenes(CharacterTestSuite::kAll);
    assert(all.count==12 && all.guitar_cache);
    bool seen[12]{};
    for(unsigned i=0;i<all.count;i++) {
        const auto index=static_cast<unsigned>(all.scenes[i]);
        assert(index<12 && !seen[index]);seen[index]=true;
        assert(CharacterPreviewDurationMs(all.scenes[i])>0);
    }
    assert(CharacterTestScenes(CharacterTestSuite::kRemaining).count==7);
    assert(CharacterTestScenes(CharacterTestSuite::kTheatre).count==2);
    assert(CharacterTestScenes(CharacterTestSuite::kBaseline).count==3);
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
    unsigned rendered = 0;
    for(int scene=0;scene<12;scene++){
        for(unsigned i=0;i<=CharacterPreviewDurationMs(scenes[scene])*30/1000;i++) {
            assert(RenderCharacterPreview(frame,kCharacterBytes,scenes[scene],i/30.f));
            ++rendered;
            for(size_t j=0;j<16;j++){assert(storage[j]==0xa5);assert(storage[kCharacterBytes+16+j]==0xa5);}
        }
        if(scene>=3) {
            std::vector<uint8_t> neutral(kCharacterBytes);
            assert(RenderCharacterPreview(neutral.data(),neutral.size(),CharacterPreview::kEyes,0));
            for(float t : {0.f,CharacterPreviewDurationMs(scenes[scene])/1000.f,100.f}) {
                assert(RenderCharacterPreview(frame,kCharacterBytes,scenes[scene],t));
                if(scenes[scene]!=CharacterPreview::kRub || t==0)
                    assert(std::memcmp(frame,neutral.data(),kCharacterBytes)==0);
                else {
                    // Rub ends in sleepy eyes, not neutral; the paw is gone.
                    assert(std::memcmp(frame,neutral.data(),kCharacterBytes)!=0);
                    for(int pixel=220*360;pixel<360*360;pixel++)assert(frame[2*pixel]==8 && frame[2*pixel+1]==0x82);
                }
            }
        }
        const float sample[]={1.2f,1.2f,1.2f,4.5f,3.1f,2.2f,2.f,2.4f,2.6f,2.f,2.5f,2.1f};
        assert(RenderCharacterPreview(frame,kCharacterBytes,scenes[scene],sample[scene]));
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
    std::printf("PASS: %u rendered frames; 540 cached guitar frames byte-identical; theatre entry/exit clean; bounds, alpha, byte order and invalid inputs\n",rendered);
}
