package com.nouprax.markdown.core

/** A polite busy-wait pause while another thread finishes a bounded native
 * call; the wait window is one scope-table materialization. */
internal expect fun materializeWaitHint()
