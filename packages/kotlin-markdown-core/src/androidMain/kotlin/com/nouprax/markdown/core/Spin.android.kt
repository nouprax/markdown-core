@file:kotlin.jvm.JvmName("FootnoteQueriesKt")
@file:kotlin.jvm.JvmMultifileClass

package com.nouprax.markdown.core

import kotlin.jvm.JvmSynthetic

@JvmSynthetic
internal actual fun materializeWaitHint() {
    // Thread.onSpinWait needs API 30+/ART support; yielding is the portable
    // pause for this bounded window.
    Thread.yield()
}
