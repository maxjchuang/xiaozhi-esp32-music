#pragma once
#include <cstddef>
#include <cstdint>

namespace anim {
enum class CharacterPreview { kEyes, kWave, kGuitar };
// Portable renderer; caller owns a 360x360 RGB565A8 buffer. RGB565 bytes are
// swapped to match the existing ST77916 image path, alpha is always opaque.
constexpr int kCharacterSize = 360;
constexpr size_t kCharacterBytes = kCharacterSize * kCharacterSize * 3;
bool RenderCharacterPreview(uint8_t* buffer, size_t size,
                            CharacterPreview scene, float seconds);
// Cache contains the opaque guitar scene without the moving strumming paw.
bool RenderGuitarBase(uint8_t* buffer, size_t size);
// Buffers must be distinct and at least kCharacterBytes bytes long.
bool RenderCachedGuitar(uint8_t* buffer, size_t size,
                       const uint8_t* base, size_t base_size, float seconds);
}  // namespace anim
