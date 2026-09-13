package com.nouprax.markdown.core

public class Text internal constructor(
    public val literal: String,
    override val scope: Scope,
    override val anchor: String?,
    override val attributes: Attributes,
) : Markup
