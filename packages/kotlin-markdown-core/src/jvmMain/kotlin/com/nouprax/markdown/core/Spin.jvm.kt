package com.nouprax.markdown.core

internal actual fun materializeWaitHint() {
    Thread.onSpinWait()
}
