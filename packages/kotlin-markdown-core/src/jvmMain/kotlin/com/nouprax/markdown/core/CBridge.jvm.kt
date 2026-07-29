package com.nouprax.markdown.core

internal actual fun ensureNativeLoaded() = DesktopNativeLoader.ensureLoaded()

private object DesktopNativeLoader {
    private val loaded: Unit = load()

    fun ensureLoaded() = loaded

    private fun load() {
        val resource =
            HostNativeLibrary.resourcePath
                ?: throw UnsupportedOperationException(
                    "unsupported native platform: ${HostNativeLibrary.hostName}",
                )
        val stream =
            requireNotNull(DesktopNativeLoader::class.java.getResourceAsStream(resource)) {
                "native library is missing for ${HostNativeLibrary.platform}"
            }
        HostNativeLibrary.extractAndLoad(stream)
    }
}
