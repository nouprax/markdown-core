@file:kotlin.jvm.JvmName("MarkdownCoreKt")
@file:kotlin.jvm.JvmMultifileClass

package com.nouprax.markdown.core

import kotlin.jvm.JvmSynthetic

/**
 * A native document's address, handed back inside the payload that describes
 * it. Zero is never a valid handle.
 */
internal typealias CDocumentHandle = Long

/** Parses [source] and returns the MKC4 payload describing the document. */
@JvmSynthetic
internal expect fun cOpen(
    source: ByteArray,
    options: ParseOptions,
): ByteArray

/**
 * Hands [source] to the document behind this handle and returns the MKC4
 * payload describing its successor. CONSUMES the handle on every path,
 * success or not — the successor's handle arrives inside the payload.
 */
@JvmSynthetic
internal expect fun CDocumentHandle.edit(source: ByteArray): ByteArray

/** Releases a handle no edit has consumed. */
@JvmSynthetic
internal expect fun CDocumentHandle.release()

/**
 * Arranges for [release] to run once [owner] becomes unreachable, and returns
 * the registration for [owner] to hold.
 *
 * A [Document] owns a native parse and common Kotlin has no deterministic
 * destructor, so the backstop is the platform's own reclaim mechanism:
 * `java.lang.ref.Cleaner` on the JVM and Android, `createCleaner` on Native.
 * [Document.close] remains the way to release promptly; this is what keeps a
 * dropped document from leaking, and it is never a reason not to close one.
 *
 * The registration is returned because Kotlin/Native ties the cleanup to the
 * lifetime of the cleaner OBJECT, so the owner must hold it; the JVM ties it
 * to [owner] itself and needs nothing held. What neither may do is let
 * [release] reach [owner] — an action that reaches its own owner keeps that
 * owner reachable, and then it never runs.
 */
@JvmSynthetic
internal expect fun attachNativeCleanup(
    owner: Any,
    release: () -> Unit,
): Any?
