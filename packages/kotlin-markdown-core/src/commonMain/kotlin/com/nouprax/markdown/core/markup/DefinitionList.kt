package com.nouprax.markdown.core

public class DefinitionList internal constructor(
    public val definitions: kotlin.collections.List<Definition>,
    override val scope: Scope,
    override val anchor: String?,
    override val attributes: Attributes,
) : Markup
