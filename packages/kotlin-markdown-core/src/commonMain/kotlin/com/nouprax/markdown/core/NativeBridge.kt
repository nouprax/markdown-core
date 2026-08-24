package com.nouprax.markdown.core

internal expect fun nativeParse(
    source: ByteArray,
    options: ParseOptions,
): ByteArray
