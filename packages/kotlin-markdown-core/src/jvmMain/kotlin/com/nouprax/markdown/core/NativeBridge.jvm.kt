package com.nouprax.markdown.core

import java.nio.file.Files
import java.nio.file.StandardCopyOption

internal actual fun nativeParse(
    source: ByteArray,
    options: ParseOptions,
): ByteArray {
    DesktopNativeLoader.ensureLoaded()
    return JvmNative.parse(source, options.toNativeMask())
}

internal object JvmNative {
    external fun parse(
        source: ByteArray,
        optionsMask: Int,
    ): ByteArray
}

private object DesktopNativeLoader {
    private val loaded: Unit = load()

    fun ensureLoaded() = loaded

    private fun load() {
        val os = System.getProperty("os.name").lowercase()
        val architecture = System.getProperty("os.arch").lowercase()
        val platform =
            when {
                os.contains("mac") && architecture in setOf("aarch64", "arm64") -> "macos-arm64"
                os.contains("mac") && architecture in setOf("x86_64", "amd64") -> "macos-x64"
                os.contains("linux") && architecture in setOf("x86_64", "amd64") -> "linux-x64"
                os.contains("windows") && architecture in setOf("x86_64", "amd64") -> "windows-x64"
                else -> throw UnsupportedOperationException("unsupported native platform: $os/$architecture")
            }
        val filename =
            when {
                os.contains("mac") -> "libmarkdown_core_kotlin.dylib"
                os.contains("windows") -> "markdown_core_kotlin.dll"
                else -> "libmarkdown_core_kotlin.so"
            }
        val resource = "/com/nouprax/markdown/core/native/$platform/$filename"
        val input =
            requireNotNull(DesktopNativeLoader::class.java.getResourceAsStream(resource)) {
                "native library is missing for $platform"
            }
        val directory = Files.createTempDirectory("markdown-core-")
        val library = directory.resolve(filename)
        input.use { Files.copy(it, library, StandardCopyOption.REPLACE_EXISTING) }
        library.toFile().deleteOnExit()
        directory.toFile().deleteOnExit()
        System.load(library.toAbsolutePath().toString())
    }
}
