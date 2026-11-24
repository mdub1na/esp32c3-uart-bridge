#!/bin/sh
set -eu

mode=${1:-normal}
case "$mode" in
    normal|loopback|luatos|luatos-recovery) ;;
    *) echo "Usage: scripts/build.sh [normal|loopback|luatos|luatos-recovery]" >&2; exit 2 ;;
esac

repo_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
: "${IDF_PATH:?Activate the ESP-IDF environment with its export.sh first}"
expected=$(python -c 'import json,sys; print(json.load(open(sys.argv[1]))["esp_idf_commit"])' "$repo_dir/idf.lock.json")
actual=$(git -C "$IDF_PATH" rev-parse HEAD)
if [ "$actual" != "$expected" ]; then
    echo "ESP-IDF revision differs from idf.lock.json; build stopped." >&2
    exit 1
fi

defaults="$repo_dir/sdkconfig.defaults"
expect_loopback=OFF
expect_luatos=OFF
expect_luatos_recovery=OFF
if [ "$mode" = loopback ]; then
    defaults="$defaults;$repo_dir/sdkconfig.loopback"
    expect_loopback=ON
elif [ "$mode" = luatos ] || [ "$mode" = luatos-recovery ]; then
    defaults="$defaults;$repo_dir/sdkconfig.luatos"
    expect_luatos=ON
    if [ "$mode" = luatos-recovery ]; then
        defaults="$defaults;$repo_dir/sdkconfig.luatos-recovery"
        expect_luatos_recovery=ON
    fi
fi

idf.py -C "$repo_dir" -B "$repo_dir/build/$mode" \
    -D "IDF_TARGET=esp32c3" \
    -D "BRIDGE_EXPECT_INTERNAL_LOOPBACK=$expect_loopback" \
    -D "BRIDGE_EXPECT_LUATOS=$expect_luatos" \
    -D "BRIDGE_EXPECT_LUATOS_RECOVERY=$expect_luatos_recovery" \
    -D "SDKCONFIG=$repo_dir/build/$mode/sdkconfig" \
    -D "SDKCONFIG_DEFAULTS=$defaults" build
