package com.nouprax.markdown.core

/**
 * The reader of the MCB3 answers of a session's queries
 * (docs/architecture/wire-format.md, Queries), shared by every Kotlin target
 * as [WireDecoder] is. A failure's message throws what it carries.
 */
internal object AnswerDecoder {
    /** The scopes of [message]. */
    fun scopes(message: ByteArray): kotlin.collections.List<Scope> {
        val reader = Reader(message)
        return reader.list { reader.scope() }
    }

    private class Reader(
        private val message: ByteArray,
    ) {
        private var offset = WireDecoder.STATUS_OFFSET

        init {
            if (u8() == WireDecoder.FAILURE) throw WireDecoder.failure(count())
        }

        fun scope(): Scope = Scope(Position(i32(), i32()), Position(i32(), i32()))

        inline fun <T> list(read: () -> T): kotlin.collections.List<T> = kotlin.collections.List(count()) { read() }

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
