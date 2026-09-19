#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."
report_dir=$(mktemp -d "${TMPDIR:-/tmp}/echoear-cat-coverage.XXXXXX")
echo "Artifacts: $report_dir"
if rg -n 'gfx_anim_[a-z_]+\(obj_anim_eye|mmap_assets_get_(mem|size)\([^,]+, MMAP_EMOJI_NORMAL_[A-Z_]+_EAF' main/boards/echoear/emote_display.cc; then
    echo 'FAIL: legacy eye decoder entry point remains' >&2
    exit 1
fi
"${CXX:-clang++}" -std=c++17 -Wall -Wextra -Werror -Wno-missing-field-initializers \
    -fsanitize=address,undefined -DCONFIG_ECHOEAR_IDLE_THEATRE_TRIAL=1 \
    -DCONFIG_ECHOEAR_IDLE_MICRO_MOTIONS=1 -DCONFIG_ECHOEAR_IDLE_SLEEP_TIMEOUT_SECONDS=300 \
    -I docs/prototypes/director-test-stubs -I main/boards/echoear -I main \
    docs/prototypes/test-character-coverage.cc main/boards/echoear/expression_director.cc \
    main/boards/echoear/character_preview.cc -o "$report_dir/coverage"
"$report_dir/coverage" "$report_dir"
bash docs/prototypes/test-expression-theatre.sh
for test in idle-theatre music-companion-preferences; do
    "${CXX:-clang++}" -std=c++17 -Wall -Wextra -Werror -fsanitize=address,undefined \
        -I main/boards/echoear "docs/prototypes/test-$test.cc" -o "$report_dir/$test"
    "$report_dir/$test"
done
if command -v ffmpeg >/dev/null; then
    ffmpeg -y -framerate 1 -i "$report_dir/pose-%d.ppm" -vf 'scale=180:180,tile=6x4' \
        -frames:v 1 "$report_dir/contact.png" -loglevel error
fi
echo "PASS: host coverage; device/ASR/physical display are separate checks"
