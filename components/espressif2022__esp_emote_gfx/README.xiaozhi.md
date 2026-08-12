# Local esp_emote_gfx override

This directory vendors `espressif2022/esp_emote_gfx` version `1.2.0~1`
(upstream commit `7ee69b15da906f8c03e3ee9e38ec9732417f9445`). It is kept as a local
component so the EchoEar build does not lose the following fixes when ESP-IDF
regenerates `managed_components/`:

- render only when at least one display object is dirty;
- refresh only the dirty object bounds for static widgets while keeping
  animations on full-frame refreshes;
- apply the dirty rectangle's horizontal and vertical source offsets when
  drawing images;
- apply the horizontal mask offset when drawing labels;
- ignore hidden objects when calculating the dirty region and clear an image
  object's dirty flag after rendering.

Without these changes, a static full-screen music background is redrawn at the
global graphics frame rate. On EchoEar that can starve audio/network tasks and
eventually trigger the task watchdog. Incorrect source offsets also cause
rectangular seams and clipped lyrics when the rotating disc is refreshed.
