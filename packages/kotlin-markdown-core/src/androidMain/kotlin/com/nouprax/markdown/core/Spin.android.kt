package com.nouprax.markdown.core

internal actual fun materializeWaitHint() {
    // Thread.onSpinWait needs API 30+/ART support; yielding is the portable
    // pause for this bounded window.
    Thread.yield()
}
