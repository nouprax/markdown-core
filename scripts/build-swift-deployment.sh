#!/bin/sh
set -eu

platform=${1:?usage: build-swift-deployment.sh PLATFORM VERSION}
version=${2:?usage: build-swift-deployment.sh PLATFORM VERSION}

case "$platform" in
    iOS)
        destination="generic/platform=iOS"
        deployment_setting="IPHONEOS_DEPLOYMENT_TARGET=$version"
        ;;
    macOS)
        destination="generic/platform=macOS"
        deployment_setting="MACOSX_DEPLOYMENT_TARGET=$version"
        ;;
    *)
        echo "unsupported platform: $platform" >&2
        exit 2
        ;;
esac

xcodebuild \
    -scheme swift-markdown-core-Package \
    -destination "$destination" \
    -derivedDataPath ".build/xcode-$platform-$version" \
    "$deployment_setting" \
    CODE_SIGNING_ALLOWED=NO \
    build
