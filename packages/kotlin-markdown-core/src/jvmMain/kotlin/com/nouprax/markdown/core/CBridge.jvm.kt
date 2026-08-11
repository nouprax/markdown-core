@file:kotlin.jvm.JvmName("MarkdownCoreKt")
@file:kotlin.jvm.JvmMultifileClass

package com.nouprax.markdown.core

import kotlin.jvm.JvmSynthetic

@JvmSynthetic
internal actual fun ensureNativeLoaded() = DesktopNativeLoader.ensureLoaded()

private object DesktopNativeLoader {
    private val loaded: Unit = load()

    fun ensureLoaded() = loaded

    private fun load() {
        val resource =
            hostNativeLibraryResourcePath
                ?: throw UnsupportedOperationException(
                    "unsupported native platform: $hostNativeLibraryHostName",
                )
        val stream =
            requireNotNull(DesktopNativeLoader::class.java.getResourceAsStream(resource)) {
                "native library is missing for $hostNativeLibraryPlatform"
            }
        extractAndLoadHostNativeLibrary(stream)
    }
}
