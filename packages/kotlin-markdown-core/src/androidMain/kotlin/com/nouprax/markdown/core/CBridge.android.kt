@file:kotlin.jvm.JvmName("FootnoteQueriesKt")
@file:kotlin.jvm.JvmMultifileClass

package com.nouprax.markdown.core

import kotlin.jvm.JvmSynthetic

@JvmSynthetic
internal actual fun ensureNativeLoaded() = AndroidNativeLoader.ensureLoaded()

private object AndroidNativeLoader {
    private val loaded: Unit = load()

    fun ensureLoaded() = loaded

    private fun load() {
        if (System.getProperty("java.vm.name").orEmpty().contains("Dalvik", ignoreCase = true)) {
            System.loadLibrary("markdown_core_kotlin")
            return
        }
        // Host-JVM execution (Robolectric and other local unit tests). Three
        // supported layers: an explicit library path, a host build of the
        // library on the classpath at the desktop resource layout, and an
        // actionable failure naming both remedies.
        val explicit = System.getProperty("markdown.core.hostNativeLibrary")
        if (explicit != null) {
            System.load(explicit)
            return
        }
        val resource = hostNativeLibraryResourcePath
        val stream = resource?.let { AndroidNativeLoader::class.java.getResourceAsStream(it) }
        if (stream == null) {
            val fileName = hostNativeLibraryFileName
            throw IllegalStateException(
                "Markdown Core's Android artifact is running on a host JVM without its native " +
                    "library. Either set -Dmarkdown.core.hostNativeLibrary=/path/to/$fileName " +
                    "to a host build of markdown_core_kotlin, or put a host build on the test " +
                    "classpath at ${resource ?: "com/nouprax/markdown/core/native/<platform>/$fileName"}.",
            )
        }
        extractAndLoadHostNativeLibrary(stream)
    }
}
