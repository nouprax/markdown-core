#!/usr/bin/env bash
set -euo pipefail

artifact_dir=${1:-}
suite=${2:-}
page_size=${3:-}

case "$suite" in
    correctness)
        runner_arguments=(-e notClass com.nouprax.markdown.core.AstTest)
        ;;
    conformance)
        runner_arguments=(-e class com.nouprax.markdown.core.AstTest)
        ;;
    *)
        echo "usage: $0 <artifact-dir> correctness|conformance 4k|16k" >&2
        exit 2
        ;;
esac

case "$page_size" in
    4k) system_image_source=google_apis ;;
    16k) system_image_source=google_apis_ps16k ;;
    *)
        echo "usage: $0 <artifact-dir> correctness|conformance 4k|16k" >&2
        exit 2
        ;;
esac

test -d "$artifact_dir"
(
    cd "$artifact_dir"
    sha256sum --check SHA256SUMS
)
grep -Fxq 'kind=android-instrumentation-apk' "$artifact_dir/manifest.txt"
if [ -n "${GITHUB_SHA:-}" ]; then
    grep -Fxq "source_sha=$GITHUB_SHA" "$artifact_dir/manifest.txt"
fi

apk="$artifact_dir/kotlin-markdown-core-androidTest.apk"
test -f "$apk"

: "${ANDROID_HOME:?ANDROID_HOME must point to the Android SDK}"
avdmanager="$ANDROID_HOME/cmdline-tools/latest/bin/avdmanager"
emulator="$ANDROID_HOME/emulator/emulator"
adb="$ANDROID_HOME/platform-tools/adb"
for tool in "$avdmanager" "$emulator" "$adb"; do
    test -x "$tool"
done

avd_name="markdown-core-api36-${page_size}-${GITHUB_RUN_ID:-local}-${GITHUB_RUN_ATTEMPT:-1}"
system_image="system-images;android-36;${system_image_source};x86_64"
serial="emulator-5554"
diagnostic_dir=${RUNNER_TEMP:-/tmp}/android-test-${page_size}-${suite}
mkdir -p "$diagnostic_dir"
export ANDROID_AVD_HOME="$diagnostic_dir/avd"
mkdir -p "$ANDROID_AVD_HOME"

cleanup() {
    set +e
    timeout 10s "$adb" -s "$serial" logcat -d >"$diagnostic_dir/logcat.txt" 2>&1
    timeout 10s "$adb" -s "$serial" shell getprop >"$diagnostic_dir/getprop.txt" 2>&1
    timeout 10s "$adb" -s "$serial" emu kill >/dev/null 2>&1
    if [ -n "${emulator_pid:-}" ]; then
        if kill -0 "$emulator_pid" 2>/dev/null; then
            kill "$emulator_pid" 2>/dev/null
        fi
        wait "$emulator_pid" 2>/dev/null
    fi
    rm -rf "$ANDROID_AVD_HOME/$avd_name.avd" "$ANDROID_AVD_HOME/$avd_name.ini"
}
trap cleanup EXIT

printf 'no\n' | "$avdmanager" create avd \
    --force \
    --name "$avd_name" \
    --package "$system_image" \
    --device pixel \
    --path "$ANDROID_AVD_HOME/$avd_name.avd" \
    | tee "$diagnostic_dir/avdmanager.txt"
if ! "$emulator" -list-avds | grep -Fxq "$avd_name"; then
    echo "avdmanager did not create the requested AVD: $avd_name" >&2
    "$emulator" -list-avds >&2
    exit 1
fi

"$emulator" "@$avd_name" \
    -port 5554 \
    -accel on \
    -gpu swiftshader \
    -no-audio \
    -no-boot-anim \
    -no-metrics \
    -no-snapshot \
    -no-window \
    -wipe-data \
    >"$diagnostic_dir/emulator.txt" 2>&1 &
emulator_pid=$!

for _ in $(seq 1 180); do
    if ! kill -0 "$emulator_pid" 2>/dev/null; then
        echo "Android emulator exited before adb connected" >&2
        sed -n '1,320p' "$diagnostic_dir/emulator.txt" >&2
        exit 1
    fi
    if [ "$("$adb" -s "$serial" get-state 2>/dev/null || true)" = device ]; then
        break
    fi
    sleep 1
done
if [ "$("$adb" -s "$serial" get-state 2>/dev/null || true)" != device ]; then
    echo "Android emulator did not connect to adb within three minutes" >&2
    exit 1
fi
for _ in $(seq 1 180); do
    if ! kill -0 "$emulator_pid" 2>/dev/null; then
        echo "Android emulator exited before boot completed" >&2
        sed -n '1,320p' "$diagnostic_dir/emulator.txt" >&2
        exit 1
    fi
    if [ "$("$adb" -s "$serial" shell getprop sys.boot_completed 2>/dev/null | tr -d '\r')" = 1 ]; then
        break
    fi
    sleep 2
done
if [ "$("$adb" -s "$serial" shell getprop sys.boot_completed 2>/dev/null | tr -d '\r')" != 1 ]; then
    echo "Android emulator did not boot within six minutes" >&2
    exit 1
fi

"$adb" -s "$serial" shell settings put global window_animation_scale 0
"$adb" -s "$serial" shell settings put global transition_animation_scale 0
"$adb" -s "$serial" shell settings put global animator_duration_scale 0
"$adb" -s "$serial" logcat -c
"$adb" -s "$serial" install -r -t "$apk"
"$adb" -s "$serial" shell pm list instrumentation | tee "$diagnostic_dir/instrumentation.txt"

instrumentation_output="$diagnostic_dir/instrumentation-${suite}.txt"
# Bounded like the boot phases: a wedged emulator stack can hang
# `am instrument -w` indefinitely, which the caller's fresh-AVD retry can
# only absorb as a failure, never as a hang riding out the job timeout.
# The suites finish in well under two minutes on a healthy emulator.
timeout 480s "$adb" -s "$serial" shell am instrument -w -r \
    "${runner_arguments[@]}" \
    com.nouprax.markdown.core.test/androidx.test.runner.AndroidJUnitRunner \
    | tee "$instrumentation_output"

grep -Fq 'INSTRUMENTATION_CODE: -1' "$instrumentation_output"
if grep -Eq 'FAILURES!!!|INSTRUMENTATION_FAILED|Process crashed' "$instrumentation_output"; then
    echo "Android instrumentation reported a test failure" >&2
    exit 1
fi
