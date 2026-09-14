package com.nouprax.markdown.core

/**
 * The one list the decoders hand out: an array the decoder filled, wrapped
 * once and never copied. It is random access, its iterator is the base
 * class's index loop, and a typed view of the same array is a cast after
 * every element has been checked, not a second list.
 */
private class ReadOnlyList<out Element>(
    private val elements: Array<out Any?>,
) : AbstractList<Element>(),
    RandomAccess {
    override val size: Int
        get() = elements.size

    @Suppress("UNCHECKED_CAST")
    override fun get(index: Int): Element {
        if (index < 0 || index >= elements.size) {
            throw IndexOutOfBoundsException("index: $index, size: ${elements.size}")
        }
        return elements[index] as Element
    }
}

private val emptyElements: Array<Any?> = arrayOfNulls(0)

/**
 * Wraps [elements] as a read-only list without copying. The caller hands
 * over the array and never writes to it again.
 */
internal fun <Element> ownedList(elements: Array<out Any?>): kotlin.collections.List<Element> =
    if (elements.isEmpty()) emptyOwnedList() else ReadOnlyList(elements)

private val emptyOwnedList: kotlin.collections.List<Nothing> = ReadOnlyList(emptyElements)

@Suppress("UNCHECKED_CAST")
internal fun <Element> emptyOwnedList(): kotlin.collections.List<Element> =
    emptyOwnedList as kotlin.collections.List<Element>

internal fun <Element, Result> kotlin.collections.List<Element>.immutableMap(
    transform: (Element) -> Result,
): kotlin.collections.List<Result> {
    val count = size
    if (count == 0) return emptyOwnedList()
    val elements = arrayOfNulls<Any?>(count)
    for (index in 0 until count) elements[index] = transform(this[index])
    return ReadOnlyList(elements)
}

internal fun <Element> immutableList(
    size: Int,
    initializer: (Int) -> Element,
): kotlin.collections.List<Element> {
    if (size == 0) return emptyOwnedList()
    val elements = arrayOfNulls<Any?>(size)
    for (index in 0 until size) elements[index] = initializer(index)
    return ReadOnlyList(elements)
}
