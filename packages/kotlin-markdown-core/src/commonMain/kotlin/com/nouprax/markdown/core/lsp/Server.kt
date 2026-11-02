package com.nouprax.markdown.core.lsp

import com.nouprax.markdown.core.PlatformServer
import com.nouprax.markdown.core.TextUnit
import com.nouprax.markdown.core.newPlatformServer

/**
 * The language engine (docs/plans/2026-10-23-lsp-completion-engine.md,
 * section 4): the semantics of the Language Server Protocol 3.17 over
 * Markdown Core, called in process. Each method is one protocol method, and
 * answers as the C engine `markdown-core-lsp` does, so this server, the
 * other bindings and the JSON-RPC adapter `markdown-core-server` give the
 * same answers.
 *
 * A server holds the open documents of one client. It does no I/O: the host
 * hands it every text. It has one writer at a time.
 *
 * A method that fails throws [ServerException], whose [ServerException.code]
 * is the protocol's error code, and changes nothing, except an [update] that
 * fails with [ServerException.Code.INTERNAL_ERROR], which closes the
 * document.
 *
 * [close] without a URI releases the engine's server; a call after it throws
 * [IllegalStateException]. [close] with a URI is the didClose notification.
 *
 * @throws ServerException [ServerException.Code.INTERNAL_ERROR] when the
 *   server cannot be allocated.
 */
public class Server : AutoCloseable {
    private val server: PlatformServer =
        newPlatformServer() ?: throw ServerException(ServerException.Code.INTERNAL_ERROR)
    private var closed = false

    /**
     * Where the server is in the protocol's lifecycle. A host that reads
     * `exit` ends with code 0 when it is [Phase.HALTED], and 1 otherwise.
     */
    public val phase: Phase
        get() = Phase.entries[open().phase()]

    /**
     * The initialize request. [units] are the position encodings the client
     * offers, which the protocol's `general.positionEncodings` lists. The
     * server becomes [Phase.ACTIVE].
     *
     * @return the position encoding, UTF-8 when the client offers it and
     *   UTF-16 otherwise, and the completion trigger characters.
     * @throws ServerException [ServerException.Code.INVALID_REQUEST] after the
     *   first initialize.
     */
    public fun initialize(units: List<TextUnit>): Capabilities {
        val answer = IntArray(2)
        val cues = open().initialize(BooleanArray(units.size) { units[it] == TextUnit.UTF16 }, answer)
        pass(answer[0])
        return Capabilities(if (answer[1] == 1) TextUnit.UTF16 else TextUnit.UTF8, cues.decodeToString())
    }

    /** The shutdown request. The server becomes [Phase.HALTED] and takes no more calls. */
    public fun shutdown(): Unit = pass(open().shutdown())

    /**
     * The didOpen notification: the document at [uri] holds [text] at
     * [version]. A document opened again replaces the one open at its URI.
     * The server parses the text into the document's session, which each
     * later change edits.
     */
    public fun open(
        uri: String,
        version: Int,
        text: String,
    ): Unit = pass(open().open(uri.encodeToByteArray(), version, text.encodeToByteArray()))

    /**
     * The didChange notification: [edits], the content changes, applied to
     * the document in order, each to the text the one before it made, which
     * give it [version].
     *
     * @throws ServerException [ServerException.Code.INVALID_PARAMS] when the
     *   document is not open or an edit's start is after its end, which every
     *   edit is checked for before the first applies; the document then
     *   keeps its text and version. [ServerException.Code.INTERNAL_ERROR]
     *   when an edit cannot allocate, and then the server closes the
     *   document, since it holds some of the edits and cannot tell the
     *   client's text.
     */
    public fun update(
        uri: String,
        version: Int,
        edits: List<Edit>,
    ) {
        val texts = edits.map { it.text.encodeToByteArray() }
        val text = ByteArray(texts.sumOf { it.size })
        val ends = IntArray(edits.size)
        val scopes = IntArray(edits.size * 4)
        var filled = 0
        for ((index, edit) in edits.withIndex()) {
            texts[index].copyInto(text, filled)
            filled += texts[index].size
            ends[index] = filled
            // -1 is the engine's UINT32_MAX: an edit with no range replaces the whole text.
            val scope = edit.scope
            scopes[index * 4] = scope?.start?.line ?: 0
            scopes[index * 4 + 1] = scope?.start?.character ?: 0
            scopes[index * 4 + 2] = scope?.end?.line ?: -1
            scopes[index * 4 + 3] = scope?.end?.character ?: -1
        }
        pass(open().update(uri.encodeToByteArray(), version, scopes, text, ends))
    }

    /** The didClose notification. */
    public fun close(uri: String): Unit = pass(open().close(uri.encodeToByteArray()))

    /**
     * The completion request at [position] in the document at [uri]. It
     * locates the document's session at the cursor and reads the context
     * there.
     *
     * @return the completions, computed for the document's current version.
     *   A host drops completions whose version is older than the document's,
     *   as the protocol's `ContentModified` does.
     */
    public fun complete(
        uri: String,
        position: Position,
    ): Completions {
        val answer = LongArray(2)
        pass(open().complete(uri.encodeToByteArray(), position.line, position.character, answer))
        return Completions(answer[0].toInt(), answer[1].toInt())
    }

    /** Releases the engine's server and every document it holds; a second call does nothing. */
    override fun close() {
        if (closed) return
        closed = true
        server.free()
    }

    private fun open(): PlatformServer {
        check(!closed) { "the server is closed" }
        return server
    }

    private fun pass(status: Int) {
        if (status != 0) throw ServerException(ServerException.Code.entries.first { it.value == status })
    }
}

/**
 * Where a [Server] is in the protocol's lifecycle: fresh until initialize,
 * active until shutdown, and halted afterwards.
 */
public enum class Phase {
    FRESH,
    ACTIVE,
    HALTED,
}

/** What a [Server] answers initialize with. */
public data class Capabilities(
    /** The position encoding every [Position] counts in. */
    public val unit: TextUnit,
    /** The completion trigger characters, one per character: the parser's slot cues, which are ASCII. */
    public val cues: String,
)

/**
 * A protocol position: [line] and [character] counted from 0, in the
 * negotiated [Capabilities.unit], each within the protocol's `uinteger`. A
 * character past its line's end stands at the line's end, one inside a
 * character stands at the character's start, and a line past the text's last
 * stands at the text's end.
 */
public data class Position(
    public val line: Int,
    public val character: Int,
) {
    init {
        require(line >= 0 && character >= 0) { "a position counts from 0" }
    }
}

/** A protocol range: from [start] up to [end], which it does not hold. */
public data class Scope(
    public val start: Position,
    public val end: Position,
)

/** A content change of didChange: [text] replaces [scope], or the whole text when [scope] is null. */
public data class Edit(
    public val scope: Scope?,
    public val text: String,
)

/**
 * The answer to one completion request: the [version] of the document it was
 * computed for, and the [count] of its items. The items come with the
 * providers of the plan's section 5.
 */
public data class Completions(
    public val version: Int,
    public val count: Int,
)

/** Why a [Server] method failed: the protocol's error [code]. */
public class ServerException(
    public val code: Code,
) : RuntimeException(code.name) {
    /** The protocol's error code for the one reason there is no answer. */
    public enum class Code(
        /** The code's number in the protocol. */
        public val value: Int,
    ) {
        /** The call came before initialize. */
        NOT_INITIALIZED(-32002),

        /** The call is not one the server's phase takes: a second initialize, or any call after shutdown. */
        INVALID_REQUEST(-32600),

        /** The call names a document that is not open, or an edit whose start is after its end. */
        INVALID_PARAMS(-32602),

        /** An allocation failed, or a text exceeds what the parser can hold. */
        INTERNAL_ERROR(-32603),
    }
}
