package com.nouprax.markdown.core

/**
 * The reader of the MCB3 answers of a session's queries
 * (docs/architecture/wire-format.md, Queries), shared by every Kotlin target
 * as [WireDecoder] is. A failure's message throws what it carries.
 */
internal object AnswerDecoder {
    /** The context of [message], whose slots spell from [handle]. */
    fun context(
        message: ByteArray,
        handle: ContextHandle,
    ): Context {
        val reader = Reader(message)
        val blocks = reader.list { reader.opener() }
        val inlines = reader.list { reader.opener() }
        var index = 0
        val slots =
            reader.list {
                Slot(
                    SlotKind.from(reader.u8()),
                    NodeKind.from(reader.u8()),
                    reader.scope(),
                    reader.scope(),
                    closed = reader.u8() == 1,
                    prefix = reader.list { reader.string() },
                    handle = handle,
                    index = index++,
                )
            }
        return Context(blocks, inlines, slots)
    }

    /** The spelling of [message], with its caret counted in [unit]. */
    fun insertion(
        message: ByteArray,
        unit: TextUnit,
    ): Slot.Insertion? {
        val reader = Reader(message)
        if (reader.u8() == 0) return null
        val text = reader.bytes()
        val cursor = reader.count()
        // The caret stands after the value, at a scalar boundary.
        val columns = if (unit == TextUnit.UTF8) cursor else text.decodeToString(0, cursor).length
        return Slot.Insertion(text.decodeToString(), columns)
    }

    /** The scopes of [message]. */
    fun scopes(message: ByteArray): kotlin.collections.List<Scope> {
        val reader = Reader(message)
        return reader.list { reader.scope() }
    }

    /** The metadata field names and the slot cues of [message]. */
    fun dialect(message: ByteArray): Pair<kotlin.collections.List<String>, kotlin.collections.List<Char>> {
        val reader = Reader(message)
        val fields = reader.list { reader.string() }
        // Every cue is one ASCII byte.
        return fields to reader.string().toList()
    }

    private class Reader(
        private val message: ByteArray,
    ) {
        private var offset = WireDecoder.STATUS_OFFSET

        init {
            if (u8() == WireDecoder.FAILURE) throw WireDecoder.failure(count())
        }

        fun opener(): Opener {
            val kind = u8()
            return Opener(if (kind == 0) null else NodeKind.from(kind), scope())
        }

        fun scope(): Scope = Scope(Position(i32(), i32()), Position(i32(), i32()))

        inline fun <T> list(read: () -> T): kotlin.collections.List<T> = kotlin.collections.List(count()) { read() }

        fun string(): String = bytes().decodeToString()

        fun bytes(): ByteArray {
            val length = count()
            offset += length
            return message.copyOfRange(offset - length, offset)
        }

        fun count(): Int = i32()

        fun u8(): Int = message[offset++].toInt() and 0xff

        fun i32(): Int {
            var value = 0
            for (index in 0 until Int.SIZE_BYTES) {
                value =
                    value or ((message[offset + index].toInt() and 0xff) shl (index * 8))
            }
            offset += Int.SIZE_BYTES
            return value
        }
    }
}
