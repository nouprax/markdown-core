package com.nouprax.markdown.core

private class ReadOnlyList<out Element> private constructor(
    private val snapshot: kotlin.collections.List<Element>,
) : AbstractList<Element>() {
    companion object {
        /* Wraps WITHOUT copying, so the argument must be a freshly built list
         * no other holder aliases - the guarantee the private constructor used
         * to buy with a copy. Every call site passes a list it just built. */
        fun <Element> wrapping(fresh: kotlin.collections.List<Element>): ReadOnlyList<Element> = ReadOnlyList(fresh)

        fun <Element> generated(
            size: Int,
            initializer: (Int) -> Element,
        ): ReadOnlyList<Element> = ReadOnlyList(kotlin.collections.List(size, initializer))
    }

    override val size: Int
        get() = snapshot.size

    override fun get(index: Int): Element = snapshot[index]
}

internal fun <Element> kotlin.collections.List<Element>.asImmutable(): kotlin.collections.List<Element> =
    ReadOnlyList.wrapping(this)

internal fun <Element> immutableList(
    size: Int,
    initializer: (Int) -> Element,
): kotlin.collections.List<Element> = ReadOnlyList.generated(size, initializer)
