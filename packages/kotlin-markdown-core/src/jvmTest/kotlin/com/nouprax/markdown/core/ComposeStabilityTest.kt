package com.nouprax.markdown.core

import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertTrue

/** The shipped Compose stability configuration names every node kind, and only types that exist. */
class ComposeStabilityTest {
    @Test
    fun theStabilityConfigurationListsTheAstTypes() {
        val listed =
            checkNotNull(javaClass.getResource("/compose-stability.conf"))
                .readText()
                .lines()
                .map { it.trim() }
                .filter { it.isNotEmpty() && !it.startsWith("//") }
                .toSet()
        for (name in listed) {
            // A nested class is listed by its source name and loaded by its binary name.
            val binary = Regex("""\.([A-Z]\w*)\.""").replace(name) { "." + it.groupValues[1] + "$" }
            assertTrue(runCatching { Class.forName(binary) }.isSuccess, "$name names no class")
        }
        // The conformance corpus holds every kind of the contract.
        val kinds =
            canonicalAstCases
                .flatMap { NodeVisitor().also { visitor -> Document.parse(it.source).walk(visitor) }.nodes }
                .map { it.javaClass.name }
                .toSet()
        assertEquals(WireNodeKind.entries.size, kinds.size)
        for (kind in kinds) {
            assertTrue(kind in listed, "$kind is not listed as stable")
        }
    }
}
