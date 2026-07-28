package com.nouprax.markdown.core

import platform.posix.usleep

internal actual fun materializeWaitHint() {
    usleep(50u)
}
