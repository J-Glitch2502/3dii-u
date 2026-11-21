#!/usr/bin/env bash
# Run the released builder's Python CLI on Linux using its bundled Windows toolchain.
set -euo pipefail

if (( $# != 5 && $# != 6 )); then
    printf 'Usage: %s BUILDER_EXE ROM OTP SYSTEM_ARCHIVES OUTPUT [KEY_FOLDER]\n' "$0" >&2
    exit 2
fi

builder=$(realpath -e -- "$1")
args=(--pipeline --game "$(winepath -w "$(realpath -e -- "$2")")"
      --otp "$(winepath -w "$(realpath -e -- "$3")")"
      --emulator "$(winepath -w "$(realpath -e -- "$4")")"
      --out "$(winepath -w "$(realpath -m -- "$5")")")
if (( $# == 6 )); then
    args+=(--keys "$(winepath -w "$(realpath -e -- "$6")")")
fi
exec wine "$builder" "${args[@]}"
