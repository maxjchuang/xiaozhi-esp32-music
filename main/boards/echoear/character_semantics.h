#pragma once
#include "character_preview.h"
#include "mmap_generate_emoji_normal.h"
namespace anim {
// Resource IDs are retained as semantic compatibility tokens only. EchoEar
// never decodes these eye animations; all direct callers use the cat fallback.
inline CharacterPreview CharacterForAsset(int asset) {
    switch(asset) {
    case MMAP_EMOJI_NORMAL_HAPPY_EAF:
    case MMAP_EMOJI_NORMAL_WINKING_EAF: return CharacterPreview::kHappy;
    case MMAP_EMOJI_NORMAL_CONFUSED_EAF: return CharacterPreview::kConfused;
    case MMAP_EMOJI_NORMAL_SLEEP_EAF: return CharacterPreview::kSleepy;
    case MMAP_EMOJI_NORMAL_SAD_EAF:
    case MMAP_EMOJI_NORMAL_CRY_EAF: return CharacterPreview::kSad;
    case MMAP_EMOJI_NORMAL_ANGRY_EAF: return CharacterPreview::kAngry;
    case MMAP_EMOJI_NORMAL_SHOCKED_EAF: return CharacterPreview::kSurprised;
    default: return CharacterPreview::kEyes;
    }
}
}
