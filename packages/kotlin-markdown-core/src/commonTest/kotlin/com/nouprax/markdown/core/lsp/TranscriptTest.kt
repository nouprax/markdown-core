package com.nouprax.markdown.core.lsp

import com.nouprax.markdown.core.TextUnit
import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertFailsWith
import kotlin.test.assertTrue
import kotlin.test.fail

/**
 * The protocol transcripts (specs/lsp): each client message a binding can be
 * handed, replayed through [Server] in process, must get the answer the
 * JSON-RPC adapter gives. A body that is not JSON, or not a message, is the
 * adapter's alone to read, so it and its answer are left out.
 */
class TranscriptTest {
    @Test
    fun theServerAnswersEveryTranscriptAsTheAdapterDoes() {
        assertTrue(transcriptTexts.isNotEmpty())
        for (transcript in transcriptTexts) replay(transcript)
    }

    @Test
    fun editsAreCheckedWholeAndRefusedOnesLeaveTheDocumentAsItWas() {
        Server().use { server ->
            server.initialize(listOf(TextUnit.UTF8))
            server.open("file:///a.md", 1, "one\n")
            val start = Position(0, 3)
            val backwards = Edit(Scope(start, Position(0, 0)), "")
            val append = Edit(Scope(start, start), " two")
            val refused =
                assertFailsWith<ServerException> { server.update("file:///a.md", 2, listOf(append, backwards)) }
            assertEquals(ServerException.Code.INVALID_PARAMS, refused.code)
            assertEquals(1, server.complete("file:///a.md", start).version)
            server.update("file:///a.md", 3, listOf(append, Edit(null, "whole")))
            assertEquals(3, server.complete("file:///a.md", start).version)
            server.close("file:///a.md")
            val closed = assertFailsWith<ServerException> { server.complete("file:///a.md", start) }
            assertEquals(ServerException.Code.INVALID_PARAMS, closed.code)
        }
    }

    @Test
    fun aClosedServerTakesNoCall() {
        val server = Server()
        server.close()
        server.close()
        assertFailsWith<IllegalStateException> { server.phase }
    }
}

private fun replay(transcript: TranscriptText) {
    val root = Json(transcript.text).read() as Map<*, *>
    val entries = root["messages"] as List<*>
    val host = Host()
    var index = 0
    while (index < entries.size) {
        val entry = entries[index] as Map<*, *>
        index++
        if (entry.containsKey("exit")) {
            assertEquals(entry["exit"], host.exit, "${transcript.name}: the exit code")
            continue
        }
        val sent = entry["client"]
        if (sent !is Map<*, *> || !isMessage(sent)) {
            // The adapter's alone: skip its answer too.
            if (index < entries.size && (entries[index] as Map<*, *>).containsKey("server")) index++
            continue
        }
        val answer = host.handle(sent)
        val next = entries.getOrNull(index) as Map<*, *>?
        val expected = if (next != null && next.containsKey("server")) next["server"] else null
        if (expected != null) index++
        assertEquals(expected, answer, "${transcript.name}, entry $index")
    }
}

/** Whether [sent] is a request or a notification, which a binding can be handed. */
private fun isMessage(sent: Map<*, *>): Boolean {
    if (sent["jsonrpc"] != "2.0" || sent["method"] !is String) return false
    if (!sent.containsKey("id")) return true
    val id = sent["id"]
    return id is String || id is Long || id is Double
}

/** The adapter's mapping of messages onto the engine (lsp/adapter.c), written over [Server]. */
private class Host {
    val server = Server()

    val exit: Long get() = if (server.phase == Phase.HALTED) 0 else 1

    fun handle(sent: Map<*, *>): Map<String, Any?>? {
        val method = sent["method"] as String
        val params = sent["params"]
        if (!sent.containsKey("id")) {
            notify(method, params as? Map<*, *>)
            return null
        }
        val id = sent["id"]
        val code =
            try {
                return mapOf("jsonrpc" to "2.0", "id" to id, "result" to request(method, params))
            } catch (failure: ServerException) {
                failure.code.value
            } catch (failure: Refusal) {
                failure.code
            }
        return mapOf(
            "jsonrpc" to "2.0",
            "id" to id,
            "error" to mapOf("code" to code.toLong(), "message" to reason(code)),
        )
    }

    private fun request(
        method: String,
        params: Any?,
    ): Any? =
        when (method) {
            "initialize" -> {
                if (params !is Map<*, *>) throw Refusal(-32602)
                val general = (params["capabilities"] as? Map<*, *>)?.get("general") as? Map<*, *>
                val units = mutableListOf<TextUnit>()
                for (name in general?.get("positionEncodings") as? List<*> ?: emptyList<Any?>()) {
                    if (units.size == 2) break
                    if (name == "utf-8") units += TextUnit.UTF8
                    if (name == "utf-16") units += TextUnit.UTF16
                }
                val capabilities = server.initialize(units)
                mapOf(
                    "capabilities" to
                        mapOf(
                            "positionEncoding" to if (capabilities.unit == TextUnit.UTF8) "utf-8" else "utf-16",
                            "textDocumentSync" to mapOf("openClose" to true, "change" to 2L),
                            "completionProvider" to
                                mapOf("triggerCharacters" to capabilities.cues.map { it.toString() }),
                        ),
                    "serverInfo" to mapOf("name" to "markdown-core"),
                )
            }

            "shutdown" -> {
                server.shutdown()
                null
            }

            "textDocument/completion" -> {
                val fields = params as? Map<*, *>
                val uri = uri(fields)
                val position = position(fields?.get("position"))
                if (uri == null || position == null) throw Refusal(-32602)
                server.complete(uri, position)
                mapOf("isIncomplete" to false, "items" to emptyList<Any?>())
            }

            else -> {
                throw Refusal(
                    when (server.phase) {
                        Phase.FRESH -> -32002
                        Phase.HALTED -> -32600
                        Phase.ACTIVE -> -32601
                    },
                )
            }
        }

    /** A notification answers nothing; one that fails is dropped. */
    private fun notify(
        method: String,
        params: Map<*, *>?,
    ) {
        val document = params?.get("textDocument") as? Map<*, *>
        val uri = uri(params) ?: return
        try {
            when (method) {
                "textDocument/didOpen" -> {
                    val text = document?.get("text") as? String ?: return
                    val version = version(document["version"]) ?: return
                    server.open(uri, version, text)
                }

                "textDocument/didChange" -> {
                    val list = params?.get("contentChanges") as? List<*> ?: return
                    val version = version(document?.get("version")) ?: return
                    val edits = mutableListOf<Edit>()
                    for (item in list) {
                        val fields = item as? Map<*, *> ?: return
                        val text = fields["text"] as? String ?: return
                        if (fields.containsKey("range")) {
                            val range = fields["range"] as? Map<*, *> ?: return
                            val start = position(range["start"]) ?: return
                            val end = position(range["end"]) ?: return
                            edits += Edit(Scope(start, end), text)
                        } else {
                            edits += Edit(null, text)
                        }
                    }
                    server.update(uri, version, edits)
                }

                "textDocument/didClose" -> {
                    server.close(uri)
                }
            }
        } catch (dropped: ServerException) {
            // The adapter drops a notification that fails.
        }
    }

    private fun uri(params: Map<*, *>?): String? = (params?.get("textDocument") as? Map<*, *>)?.get("uri") as? String

    /**
     * A position as the adapter reads one. The adapter takes numbers up to
     * UINT32_MAX and [Position] up to the protocol's `uinteger`, 2^31 - 1;
     * the transcripts hold none between.
     */
    private fun position(value: Any?): Position? {
        val fields = value as? Map<*, *> ?: return null
        val line = integer(fields["line"], 0, Int.MAX_VALUE.toLong()) ?: return null
        val character = integer(fields["character"], 0, Int.MAX_VALUE.toLong()) ?: return null
        return Position(line.toInt(), character.toInt())
    }

    /** A document version: an int32, as the adapter reads one. */
    private fun version(value: Any?): Int? = integer(value, Int.MIN_VALUE.toLong(), Int.MAX_VALUE.toLong())?.toInt()

    /** A JSON integer, spelled in digits, within [min] to [max], as the adapter reads one. */
    private fun integer(
        value: Any?,
        min: Long,
        max: Long,
    ): Long? = (value as? Long)?.takeIf { it in min..max }

    private fun reason(code: Int): String =
        when (code) {
            -32700 -> "the message is not JSON"
            -32600 -> "the message is not a request the server takes now"
            -32601 -> "the server has no such method"
            -32602 -> "the params are not the method's"
            -32002 -> "the server is not initialized"
            else -> "the server failed"
        }
}

private class Refusal(
    val code: Int,
) : Exception()

/**
 * A reader of the transcripts' JSON: an object is a [Map], an array a
 * [List], a number spelled in digits alone a [Long] and any other a
 * [Double], as the adapter tells an integer by its spelling.
 */
private class Json(
    private val text: String,
) {
    private var at = 0

    fun read(): Any? {
        val value = value()
        space()
        if (at != text.length) fail("trailing text at $at")
        return value
    }

    private fun value(): Any? {
        space()
        return when (text[at]) {
            '{' -> {
                at++
                val members = LinkedHashMap<String, Any?>()
                space()
                if (text[at] == '}') {
                    at++
                } else {
                    while (true) {
                        space()
                        val name = string()
                        space()
                        expect(':')
                        members[name] = value()
                        space()
                        if (text[at++] == '}') break
                    }
                }
                members
            }

            '[' -> {
                at++
                val items = mutableListOf<Any?>()
                space()
                if (text[at] == ']') {
                    at++
                } else {
                    while (true) {
                        items += value()
                        space()
                        if (text[at++] == ']') break
                    }
                }
                items
            }

            '"' -> {
                string()
            }

            't' -> {
                word("true", true)
            }

            'f' -> {
                word("false", false)
            }

            'n' -> {
                word("null", null)
            }

            else -> {
                number()
            }
        }
    }

    private fun string(): String {
        expect('"')
        val out = StringBuilder()
        while (true) {
            val c = text[at++]
            when (c) {
                '"' -> {
                    return out.toString()
                }

                '\\' -> {
                    when (val escape = text[at++]) {
                        'b' -> out.append('\b')
                        'f' -> out.append('\u000C')
                        'n' -> out.append('\n')
                        'r' -> out.append('\r')
                        't' -> out.append('\t')
                        'u' -> out.append(text.substring(at, at + 4).toInt(16).toChar()).also { at += 4 }
                        else -> out.append(escape)
                    }
                }

                else -> {
                    out.append(c)
                }
            }
        }
    }

    private fun number(): Any {
        val start = at
        while (at < text.length && text[at] in "+-0123456789.eE") at++
        val spelled = text.substring(start, at)
        return Regex("-?[0-9]+").matchEntire(spelled)?.let { spelled.toLongOrNull() } ?: spelled.toDouble()
    }

    private fun word(
        spelled: String,
        value: Any?,
    ): Any? {
        if (!text.startsWith(spelled, at)) fail("not JSON at $at")
        at += spelled.length
        return value
    }

    private fun expect(c: Char) {
        if (text[at++] != c) fail("expected $c at ${at - 1}")
    }

    private fun space() {
        while (at < text.length && text[at] in " \t\r\n") at++
    }
}
