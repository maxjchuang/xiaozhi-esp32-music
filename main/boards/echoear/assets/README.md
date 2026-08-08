# EchoEar emote assets

The files in `emoji_normal/` preserve the large-eye EchoEar interface expected
by this firmware branch.

- Source: `espressif2022/esp_emote_gfx`
- Source commit: `9c66fbdc` (`feat: add assets here`)
- License: Apache License 2.0 (the source repository's `LICENSE`)

The upstream component removed this directory in commit `0a1f2e9`, while this
branch still refers to the generated `mmap_generate_emoji_normal.h` symbols.
Keeping the assets with the board makes clean builds deterministic.
