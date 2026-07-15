package consumer

import com.nouprax.markdown.core.List as MarkdownList

public fun consumeAlias(value: MarkdownList): kotlin.collections.List<String> = value.content

public fun consumeFullyQualified(value: com.nouprax.markdown.core.List): kotlin.collections.List<String> = value.content
