#!/bin/sh
# Applies a config fragment (third_party/busybox.config) to a BusyBox .config:
# every CONFIG_ option it mentions, set or "is not set", replaces the default.
# Usage: tools/configure-busybox.sh <.config> <fragment>
set -eu
config=$1
fragment=$2
grep -E '^(CONFIG_[A-Z0-9_]+=|# CONFIG_[A-Z0-9_]+ is not set)' "$fragment" | while read -r line; do
    name=$(printf '%s\n' "$line" | sed -E 's/^# (CONFIG_[A-Z0-9_]+) is not set$/\1/; s/^(CONFIG_[A-Z0-9_]+)=.*/\1/')
    grep -vE "^(${name}=|# ${name} is not set)" "$config" > "$config.tmp" || true
    printf '%s\n' "$line" >> "$config.tmp"
    mv "$config.tmp" "$config"
done
