#pragma once
#include <cstddef>
#include <cstdint>

namespace anim {
enum class CharacterPreview { kEyes, kWave, kGuitar, kBubble, kFish,
                              kChin, kRub, kHeart, kPeek, kShaker, kDrum, kKeys,
                              kStartup, kListen, kThink, kSpeak, kHappy, kConfused,
                              kSleepy, kSad, kAngry, kSurprised };
inline bool CharacterAction(const char* name, CharacterPreview& result) {
    if (!name) return false;
    const char* names[] = {"wave", "chin", "rub", "bubble", "heart", "fish", "peek",
                           "shaker", "drum", "keys", "guitar"};
    const CharacterPreview poses[] = {CharacterPreview::kWave, CharacterPreview::kChin,
        CharacterPreview::kRub, CharacterPreview::kBubble, CharacterPreview::kHeart,
        CharacterPreview::kFish, CharacterPreview::kPeek, CharacterPreview::kShaker,
        CharacterPreview::kDrum, CharacterPreview::kKeys, CharacterPreview::kGuitar};
    for (unsigned i=0; i<11; ++i) {
        unsigned j=0; while (name[j] && names[i][j] && name[j]==names[i][j]) ++j;
        if (!name[j] && !names[i][j]) { result=poses[i]; return true; }
    }
    return false;
}
constexpr unsigned CharacterPreviewDurationMs(CharacterPreview scene) {
    switch (scene) {
    case CharacterPreview::kBubble:
    case CharacterPreview::kShaker:
    case CharacterPreview::kDrum:
    case CharacterPreview::kKeys: return 7000;
    case CharacterPreview::kFish: return 6500;
    case CharacterPreview::kChin: return 5000;
    case CharacterPreview::kRub: return 5400;
    case CharacterPreview::kHeart: return 4800;
    default: return 6000; // Preserve the validated baseline test durations.
    }
}
enum class CharacterTestSuite { kBaseline, kTheatre, kRemaining, kAll };
struct CharacterSceneList { const CharacterPreview* scenes; unsigned count; bool guitar_cache; };
inline CharacterSceneList CharacterTestScenes(CharacterTestSuite suite) {
    static constexpr CharacterPreview baseline[]={CharacterPreview::kEyes,CharacterPreview::kWave,CharacterPreview::kGuitar};
    static constexpr CharacterPreview theatre[]={CharacterPreview::kBubble,CharacterPreview::kFish};
    static constexpr CharacterPreview remaining[]={CharacterPreview::kChin,CharacterPreview::kHeart,CharacterPreview::kPeek,
        CharacterPreview::kShaker,CharacterPreview::kDrum,CharacterPreview::kKeys,CharacterPreview::kRub};
    static constexpr CharacterPreview all[]={CharacterPreview::kEyes,CharacterPreview::kWave,CharacterPreview::kGuitar,
        CharacterPreview::kBubble,CharacterPreview::kFish,CharacterPreview::kChin,CharacterPreview::kHeart,CharacterPreview::kPeek,
        CharacterPreview::kShaker,CharacterPreview::kDrum,CharacterPreview::kKeys,CharacterPreview::kRub};
    switch(suite) {
    case CharacterTestSuite::kTheatre: return {theatre,2,false};
    case CharacterTestSuite::kRemaining: return {remaining,7,false};
    case CharacterTestSuite::kAll: return {all,12,true};
    default: return {baseline,3,true};
    }
}
// Portable renderer; caller owns a 360x360 RGB565A8 buffer. RGB565 bytes are
// swapped to match the existing ST77916 image path, alpha is always opaque.
constexpr int kCharacterSize = 360;
constexpr size_t kCharacterBytes = kCharacterSize * kCharacterSize * 3;
bool RenderCharacterPreview(uint8_t* buffer, size_t size,
                            CharacterPreview scene, float seconds);
// Continuous instrument companion. Caller supplies accumulated active playback
// seconds; repeat the same timestamp to freeze. No automatic seven-second exit.
bool RenderMusicCompanion(uint8_t* buffer, size_t size, double seconds,
                          CharacterPreview instrument = CharacterPreview::kShaker);
// Cache contains the opaque guitar scene without the moving strumming paw.
bool RenderGuitarBase(uint8_t* buffer, size_t size);
// Buffers must be distinct and at least kCharacterBytes bytes long.
bool RenderCachedGuitar(uint8_t* buffer, size_t size,
                       const uint8_t* base, size_t base_size, float seconds);
}  // namespace anim
