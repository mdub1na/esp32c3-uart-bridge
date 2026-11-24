#!/bin/sh
set -eu
project_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
test_binary=$(mktemp "${TMPDIR:-/tmp}/bridge-control-test.XXXXXX")
trap 'rm -f "$test_binary"' EXIT HUP INT TERM
for component in bridge_control luatos_frame d9w_recovery; do
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror -Wpedantic \
        -fsanitize=address,undefined -g -I"$project_dir/main" \
        "$project_dir/main/$component.c" \
        "$project_dir/tests/test_$component.c" -o "$test_binary"
    "$test_binary"
done
