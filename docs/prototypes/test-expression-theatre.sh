#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."
test_dir=$(mktemp -d "${TMPDIR:-/tmp}/echoear-director.XXXXXX")
trap 'rm -f "$test_dir/off" "$test_dir/on"; rmdir "$test_dir"' EXIT
for mode in off on; do
    enabled=1
    if [[ "$mode" == off ]]; then enabled=0; fi
    "${CXX:-clang++}" -std=c++17 -Wall -Wextra -Werror \
        -Wno-missing-field-initializers -fsanitize=address,undefined \
        -DCONFIG_ECHOEAR_IDLE_THEATRE_TRIAL="$enabled" \
        -DCONFIG_ECHOEAR_IDLE_MICRO_MOTIONS=1 \
        -DCONFIG_ECHOEAR_IDLE_SLEEP_TIMEOUT_SECONDS=300 \
        -I docs/prototypes/director-test-stubs -I main/boards/echoear -I main \
        docs/prototypes/test-expression-theatre.cc main/boards/echoear/expression_director.cc \
        -o "$test_dir/$mode"
    "$test_dir/$mode"
done
