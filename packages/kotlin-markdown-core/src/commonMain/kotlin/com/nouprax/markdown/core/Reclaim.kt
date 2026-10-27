package com.nouprax.markdown.core

/**
 * Runs [release] once, on a thread of the platform's choosing, after [owner]
 * becomes unreachable. The owner keeps the answer for as long as it lives,
 * and [release] must not reach the owner, or the owner never becomes
 * unreachable.
 */
internal expect fun reclaim(
    owner: Any,
    release: () -> Unit,
): Any
