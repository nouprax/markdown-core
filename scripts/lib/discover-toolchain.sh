# Shared local JDK and Android SDK discovery for repository scripts.
#
# This file is sourced by callers so the discovered environment is available
# to both Gradle and non-Gradle child processes.

if [ -z "${JAVA_HOME:-}" ]; then
    if [ -x "/opt/homebrew/opt/openjdk/libexec/openjdk.jdk/Contents/Home/bin/java" ]; then
        JAVA_HOME="/opt/homebrew/opt/openjdk/libexec/openjdk.jdk/Contents/Home"
        export JAVA_HOME
    elif [ -x "/Applications/Android Studio.app/Contents/jbr/Contents/Home/bin/java" ]; then
        JAVA_HOME="/Applications/Android Studio.app/Contents/jbr/Contents/Home"
        export JAVA_HOME
    fi
fi

if [ -z "${ANDROID_HOME:-}" ] && [ -d "$HOME/Library/Android/sdk" ]; then
    ANDROID_HOME="$HOME/Library/Android/sdk"
    export ANDROID_HOME
fi
