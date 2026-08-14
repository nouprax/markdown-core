@file:kotlin.jvm.JvmName("MarkdownCoreKt")
@file:kotlin.jvm.JvmMultifileClass

package com.nouprax.markdown.core

import java.io.File
import java.io.FileOutputStream
import java.io.InputStream
import java.lang.ref.PhantomReference
import java.lang.ref.ReferenceQueue
import java.util.Collections
import java.util.UUID
import java.util.concurrent.ConcurrentHashMap
import kotlin.jvm.JvmSynthetic

/** Loads the JNI bridge for this target before any [JvmNative] call. */
@JvmSynthetic
internal expect fun ensureNativeLoaded()

@JvmSynthetic
internal actual fun cOpen(
    source: ByteArray,
    options: ParseOptions,
): ByteArray {
    ensureNativeLoaded()
    return JvmNative.open(source, options.toNativeMask())
}

@JvmSynthetic
internal actual fun CDocumentHandle.append(
    chunk: ByteArray,
    baseline: ULong,
): ByteArray = JvmNative.append(this, chunk, baseline.toLong())

@JvmSynthetic
internal actual fun CDocumentHandle.release(): Unit = JvmNative.release(this)

/**
 * One document's registration: the owner's phantom reachability, and what to
 * run when it arrives.
 *
 * Holds [action] and never the owner. A registration that could reach its own
 * owner would keep that owner reachable, and then it would never run.
 */
private class NativeCleanup(
    owner: Any,
    private val action: () -> Unit,
    queue: ReferenceQueue<Any>,
) : PhantomReference<Any>(owner, queue) {
    fun run() = action()
}

/**
 * Runs each document's release once that document is unreachable.
 *
 * `java.lang.ref.Cleaner` is precisely this and would otherwise be the whole
 * implementation, but it reached Android only in API 33 while this artifact
 * ships to API 21. Both JVM targets therefore share the mechanism that class
 * is built from, rather than splitting the backstop per target — a split
 * would leave the Android half unreachable from the desktop suite, which is
 * how a JVM-only API came to be called on API 21 in the first place.
 *
 * One daemon thread for the process, started with the first registration.
 */
private object NativeReclaim {
    private val queue = ReferenceQueue<Any>()

    // A phantom reference is enqueued only while something still holds it, so
    // this set is what keeps a registration alive until its owner dies.
    private val live: MutableSet<NativeCleanup> =
        Collections.newSetFromMap(ConcurrentHashMap<NativeCleanup, Boolean>())

    init {
        val thread = Thread(::drain, "markdown-core-native-reclaim")
        thread.isDaemon = true
        thread.start()
    }

    @JvmSynthetic
    fun register(
        owner: Any,
        release: () -> Unit,
    ): Any = NativeCleanup(owner, release, queue).also(live::add)

    private fun drain() {
        while (true) {
            val cleanup =
                try {
                    queue.remove() as NativeCleanup
                } catch (interrupt: InterruptedException) {
                    // Asked to stop, so stop: whatever is still queued belongs
                    // to a process on its way out, and its native memory
                    // leaves with the process. A thread that refused would
                    // outlive every host that shuts its workers down politely.
                    Thread.currentThread().interrupt()
                    return
                }
            live.remove(cleanup)
            // Java 9 enqueues phantom references cleared; Android does not
            // promise that, and clearing an already-cleared reference is a
            // no-op on both.
            cleanup.clear()
            try {
                cleanup.run()
            } catch (failure: RuntimeException) {
                // One document's failed release must not end the thread and
                // take every later document's backstop with it. Cleaner
                // ignores a throwing action for the same reason.
            }
        }
    }
}

@JvmSynthetic
internal actual fun attachNativeCleanup(
    owner: Any,
    release: () -> Unit,
): Any? =
    // Handed back only because the shared signature has to serve
    // Kotlin/Native, where the cleaner object's own lifetime is the trigger.
    // Here the registration is live whether the caller holds it or not.
    NativeReclaim.register(owner, release)

private object JvmNative {
    external fun open(
        source: ByteArray,
        optionsMask: Int,
    ): ByteArray

    // The baseline rides as the ULong's bit pattern: JNI has no unsigned
    // 64-bit scalar, and the C side casts the bits straight back.
    external fun append(
        handle: Long,
        chunk: ByteArray,
        baseline: Long,
    ): ByteArray

    external fun release(handle: Long)
}

/**
 * The one description of a host build of the JNI bridge: its supported
 * platforms, its classpath resource layout, and the temp-directory
 * extraction both host loaders share.
 */
internal val hostNativeLibraryFileName: String
    @JvmSynthetic
    get() = System.mapLibraryName("markdown_core_kotlin")

/** The lowercase `os/arch` pair, for unsupported-host failures. */
internal val hostNativeLibraryHostName: String
    @JvmSynthetic
    get() =
        "${System.getProperty("os.name").orEmpty().lowercase()}/" +
            System.getProperty("os.arch").orEmpty().lowercase()

/** The resource-layout platform of this host, or null when the host is
 * outside the supported desktop matrix. */
internal val hostNativeLibraryPlatform: String?
    @JvmSynthetic
    get() =
        run {
            val os = System.getProperty("os.name").orEmpty().lowercase()
            val architecture = System.getProperty("os.arch").orEmpty().lowercase()
            when {
                os.contains("mac") && architecture in setOf("aarch64", "arm64") -> "macos-arm64"
                os.contains("mac") && architecture in setOf("x86_64", "amd64") -> "macos-x64"
                os.contains("linux") && architecture in setOf("x86_64", "amd64") -> "linux-x64"
                else -> null
            }
        }

/** The classpath location of this host's bundled library, or null on an
 * unsupported host. */
internal val hostNativeLibraryResourcePath: String?
    @JvmSynthetic
    get() =
        hostNativeLibraryPlatform?.let {
            "/com/nouprax/markdown/core/native/$it/$hostNativeLibraryFileName"
        }

/**
 * A directory only this user can reach.
 *
 * A shared temp directory is about to hold a library this process will load as
 * code, so the permissions are the boundary — an unguessable name is not one.
 * `Files.createTempDirectory` would create it owner-only in one step, but
 * `java.nio.file` reached Android only in API 26 and the Android artifact
 * compiles this source set, so the mode is set after the fact on the `java.io`
 * surface that has been there since API 1.
 *
 * That leaves a window between `mkdir` and the mode change in which the
 * directory carries the umask's permissions. Nothing of another user's can
 * survive it: the directory is verified empty once it is locked down, and a
 * name that lost the race is abandoned rather than reused.
 */
private fun createPrivateTemporaryDirectory(): File {
    val base = File(System.getProperty("java.io.tmpdir") ?: ".")
    repeat(4) {
        val directory = File(base, "markdown-core-${UUID.randomUUID()}")
        // mkdir refuses a name that already exists, so winning it means the
        // directory is this call's.
        if (!directory.mkdir()) return@repeat
        val ownerOnly =
            directory.setReadable(false, false) &&
                directory.setWritable(false, false) &&
                directory.setExecutable(false, false) &&
                directory.setReadable(true, true) &&
                directory.setWritable(true, true) &&
                directory.setExecutable(true, true)
        if (ownerOnly && directory.list()?.isEmpty() == true) return directory
        directory.delete()
    }
    throw IllegalStateException("could not create a private directory for Markdown Core's native library")
}

/** Extracts a bundled library to a self-deleting temp directory and
 * loads it; [stream] is consumed and closed. */
@JvmSynthetic
internal fun extractAndLoadHostNativeLibrary(stream: InputStream) {
    val directory = createPrivateTemporaryDirectory()
    val library = File(directory, hostNativeLibraryFileName)

    // deleteOnExit removes entries in reverse registration order, so the
    // directory must be registered before its child.
    directory.deleteOnExit()
    stream.use { input -> FileOutputStream(library).use(input::copyTo) }
    library.deleteOnExit()
    loadBundledLibrary(library)
}

@Suppress("UnsafeDynamicallyLoadedCode")
private fun loadBundledLibrary(library: File) {
    // loadLibrary cannot address a native library extracted from a JAR.
    System.load(library.absolutePath)
}
