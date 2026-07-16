#!/usr/bin/env bash
set -euo pipefail

available_devices=
available_runtimes=
for attempt in 1 2 3; do
    available_devices=$(xcrun simctl list devices available 2>&1 || true)
    available_runtimes=$(xcrun simctl list runtimes 2>&1 || true)
    if grep -q 'com.apple.CoreSimulator.SimRuntime.iOS-' <<<"$available_runtimes"; then
        break
    fi
    echo "CoreSimulator did not expose an iOS runtime (attempt $attempt/3)." >&2
    killall -9 com.apple.CoreSimulator.CoreSimulatorService 2>/dev/null || true
    sleep 2
done

runtime=$(awk '
    /com\.apple\.CoreSimulator\.SimRuntime\.iOS-/ && !/unavailable/ { value = $NF }
    END { print value }
' <<<"$available_runtimes")
if [ -z "$runtime" ]; then
    echo "No available iOS Simulator runtime is installed." >&2
    printf '%s\n' "$available_runtimes" >&2
    xcodebuild -showsdks >&2 || true
    exit 1
fi

udid=$(awk -F '[()]' '/^[[:space:]]+iPhone .* \([0-9A-F-]+\)/ { print $2; exit }' \
    <<<"$available_devices")
if [ -z "$udid" ]; then
    device_types=$(xcrun simctl list devicetypes)
    device_type=$(awk -F '[()]' '/^iPhone 17 Pro / { print $2; exit }' <<<"$device_types")
    if [ -z "$device_type" ]; then
        device_type=$(awk -F '[()]' '/^iPhone / { print $2; exit }' <<<"$device_types")
    fi
    if [ -z "$device_type" ]; then
        echo "No iPhone simulator device type is installed." >&2
        printf '%s\n' "$device_types" >&2
        exit 1
    fi
    udid=$(xcrun simctl create MarkdownCore-CI "$device_type" "$runtime")
fi

xcrun simctl boot "$udid" >/dev/null 2>&1 || true
xcrun simctl bootstatus "$udid" -b >&2
printf 'platform=iOS Simulator,id=%s\n' "$udid"
