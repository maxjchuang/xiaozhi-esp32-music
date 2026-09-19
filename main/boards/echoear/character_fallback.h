#pragma once
#include <cstdint>
namespace anim {
// Flash-resident capsule face: no PSRAM allocation, no old EAF decoder.
// Full frame also clears prior graphics on startup and failure recovery.
struct CharacterFallback { uint8_t bytes[360*360*3]{}; };
constexpr auto MakeCharacterFallback() {
    CharacterFallback image;
    auto& data=image.bytes;
    for(int y=0;y<360;y++) for(int x=0;x<360;x++) {
        const int dx=x-(x<180?121:239);
        const int dy=y<152?y-152:y>187?y-187:0;
        const bool eye=dx*dx+dy*dy<=22*22;
        const uint16_t c=eye?0xffbb:0x0882; // RGB565 ivory / dark green
        const int i=y*360+x;
        data[2*i]=c>>8; data[2*i+1]=c&255; data[360*360*2+i]=255;
    }
    return image;
}
inline constexpr auto kCharacterFallback=MakeCharacterFallback();
}
