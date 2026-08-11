@file:kotlin.jvm.JvmName("MarkdownCoreKt")
@file:kotlin.jvm.JvmMultifileClass

package com.nouprax.markdown.core

import kotlin.jvm.JvmSynthetic

private class ReadOnlyList<out Element>(
    private val snapshot: kotlin.collections.List<Element>,
) : AbstractList<Element>() {
    override val size: Int
        get() = snapshot.size

    override fun get(index: Int): Element = snapshot[index]
}

@JvmSynthetic
internal fun <Element, Result> kotlin.collections.List<Element>.immutableMap(
    transform: (Element) -> Result,
): kotlin.collections.List<Result> = ReadOnlyList(map(transform))

@JvmSynthetic
internal fun <Element> immutableList(
    size: Int,
    initializer: (Int) -> Element,
): kotlin.collections.List<Element> = ReadOnlyList(kotlin.collections.List(size, initializer))
