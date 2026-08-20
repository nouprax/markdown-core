package com.nouprax.markdown.core

private class ReadOnlyList<out Element> private constructor(
    private val snapshot: kotlin.collections.List<Element>,
) : AbstractList<Element>() {
    companion object {
        fun <Element, Result> mapped(
            elements: kotlin.collections.List<Element>,
            transform: (Element) -> Result,
        ): ReadOnlyList<Result> = ReadOnlyList(elements.map(transform))

        fun <Element> generated(
            size: Int,
            initializer: (Int) -> Element,
        ): ReadOnlyList<Element> = ReadOnlyList(kotlin.collections.List(size, initializer))
    }

    override val size: Int
        get() = snapshot.size

    override fun get(index: Int): Element = snapshot[index]
}

internal fun <Element, Result> kotlin.collections.List<Element>.immutableMap(
    transform: (Element) -> Result,
): kotlin.collections.List<Result> = ReadOnlyList.mapped(this, transform)

internal fun <Element> immutableList(
    size: Int,
    initializer: (Int) -> Element,
): kotlin.collections.List<Element> = ReadOnlyList.generated(size, initializer)
