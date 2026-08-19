# Cloud Agent base image for Markdown Core.
#
# This image supplies only the stable, host-level toolchain the repository's
# pinned versions require and that the default agent image does not already
# provide at the exact version: Node.js, pnpm, a JDK 26, the C/C++ build
# tools, and the Android SDK packages. Repository-version-pinned and
# source-derived setup (Emscripten, the frozen pnpm install, and the
# repo-managed clang-format/cmake-format/SwiftLint tools) is intentionally
# left to the `install` phase in environment.json, where it can see the
# checked-out source and stay in lockstep with the repository.
#
# The pins below mirror docs/toolchains.md and scripts/init-environment.sh.
# Keep them in sync when those files change.
FROM ubuntu:24.04

ARG NODE_VERSION=26.5.0
ARG PNPM_VERSION=11.7.0
ARG ANDROID_CMDLINE_TOOLS=13114758
ARG ANDROID_PLATFORM=android-36
ARG ANDROID_CMAKE_VERSION=3.22.1
ARG ANDROID_NDK_VERSION=28.2.13676358

ENV DEBIAN_FRONTEND=noninteractive

# System build tools. clang is the repository's default C/C++ compiler and
# resolves its C++ runtime against the newest installed GCC toolchain, so
# libstdc++-14-dev must be present for CMake's compiler probe to link.
# python3-venv/pip back the repo-managed clang-format and cmake-format venvs.
RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        build-essential \
        clang \
        libstdc++-14-dev \
        cmake \
        pkg-config \
        git \
        curl \
        ca-certificates \
        zip \
        unzip \
        xz-utils \
        python3 \
        python3-venv \
        python3-pip \
        sudo \
    && rm -rf /var/lib/apt/lists/*

# Passwordless sudo for the agent user so `install` can install any additional
# system packages non-interactively if a future dependency requires it.
RUN usermod -aG sudo ubuntu \
    && echo 'ubuntu ALL=(ALL) NOPASSWD:ALL' > /etc/sudoers.d/90-ubuntu \
    && chmod 0440 /etc/sudoers.d/90-ubuntu

# Node.js 26.5.0 (exact) installed system-wide. The repository pins this exact
# version and its checks refuse any other, so it is baked into the image
# rather than selected by a version manager at bootstrap time.
RUN curl -fsSL "https://nodejs.org/dist/v${NODE_VERSION}/node-v${NODE_VERSION}-linux-x64.tar.xz" \
        -o /tmp/node.tar.xz \
    && tar -xJf /tmp/node.tar.xz -C /usr/local --strip-components=1 \
    && rm /tmp/node.tar.xz \
    && npm config set prefix /usr/local -g \
    && npm install -g "pnpm@${PNPM_VERSION}" \
    && node --version && pnpm --version

# Temurin JDK 26 for the Gradle daemon (matches gradle/gradle-daemon-jvm.properties).
RUN curl -fsSL "https://api.adoptium.net/v3/binary/latest/26/ga/linux/x64/jdk/hotspot/normal/eclipse" \
        -o /tmp/jdk26.tar.gz \
    && mkdir -p /opt/jdk-26 \
    && tar -xzf /tmp/jdk26.tar.gz -C /opt/jdk-26 --strip-components=1 \
    && rm /tmp/jdk26.tar.gz \
    && /opt/jdk-26/bin/java -version

ENV JAVA_HOME=/opt/jdk-26
ENV ANDROID_HOME=/opt/android-sdk
ENV ANDROID_SDK_ROOT=/opt/android-sdk
# The Emscripten SDK is installed by the `install` phase into the repository's
# .tools tree at the deterministic /workspace checkout path; adding its emcc
# directory here puts emcc on PATH for the ECMAScript/WebAssembly build without
# requiring each shell to source emsdk_env.sh. The directory does not exist at
# image-build time; it is populated during bootstrap and preserved in the
# environment snapshot.
ENV PATH=/usr/local/bin:/opt/jdk-26/bin:/opt/android-sdk/cmdline-tools/latest/bin:/opt/android-sdk/platform-tools:/workspace/.tools/emsdk/4.0.23/upstream/emscripten:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin

# Android SDK packages required to configure and build the Kotlin package's
# Android Gradle Plugin target and its four-ABI JNI runtime. The Android
# Gradle Plugin refuses to configure the Kotlin Multiplatform module without an
# SDK location, so even the JVM tests need these present. The emulator and
# system images are intentionally omitted: device tests require KVM and are
# CI-only per docs/toolchains.md.
RUN mkdir -p "${ANDROID_HOME}/cmdline-tools" \
    && curl -fsSL "https://dl.google.com/android/repository/commandlinetools-linux-${ANDROID_CMDLINE_TOOLS}_latest.zip" \
        -o /tmp/cmdline-tools.zip \
    && unzip -q /tmp/cmdline-tools.zip -d /tmp/cmdline-extract \
    && mv /tmp/cmdline-extract/cmdline-tools "${ANDROID_HOME}/cmdline-tools/latest" \
    && rm -rf /tmp/cmdline-tools.zip /tmp/cmdline-extract \
    && yes | "${ANDROID_HOME}/cmdline-tools/latest/bin/sdkmanager" --licenses >/dev/null \
    && "${ANDROID_HOME}/cmdline-tools/latest/bin/sdkmanager" \
        "platform-tools" \
        "platforms;${ANDROID_PLATFORM}" \
        "cmake;${ANDROID_CMAKE_VERSION}" \
        "ndk;${ANDROID_NDK_VERSION}" \
    && chown -R ubuntu:ubuntu "${ANDROID_HOME}"

USER ubuntu
