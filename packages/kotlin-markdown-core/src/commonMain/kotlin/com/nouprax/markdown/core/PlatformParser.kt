package com.nouprax.markdown.core

internal expect fun parsePlatformDocument(
    source: ByteArray,
    options: ParseOptions,
): Document
