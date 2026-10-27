@file:OptIn(kotlin.experimental.ExperimentalNativeApi::class)

package com.nouprax.markdown.core

import kotlin.native.ref.createCleaner

internal actual fun reclaim(
    owner: Any,
    release: () -> Unit,
): Any = createCleaner(release) { it() }
