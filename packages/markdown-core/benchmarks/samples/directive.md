Inline directives:

:cite[Smith *et al.*]{id=smith class=paper data-kind=ref} :badge[stable]{class=green}
:cite[Smith *et al.*]{id=smith class=paper data-kind=ref} :badge[stable]{class=green}
:cite[Smith *et al.*]{id=smith class=paper data-kind=ref} :badge[stable]{class=green}
:cite[Smith *et al.*]{id=smith class=paper data-kind=ref} :badge[stable]{class=green}
:cite[Smith *et al.*]{id=smith class=paper data-kind=ref} :badge[stable]{class=green}

Leaf directives:

::youtube[Video of a *cat*]{vid=01ab2cd3efg class=embed class=featured}
::banner[Launch]{id=launch class=wide data-kind=hero}
::status[]{state=green}

Container directives:

::::spoiler{id=outer class=red class=green class=blue}
He dies.

:::spoiler
She is born.
:::
::::

Unclosed container:

::::spoiler
These three are not enough to close.
:::
So this line is also part of the container.

Malformed inline directives that should stay linear:

:x[:x[:x[:x[:x[:x[:x[:x[:x[:x[:x[:x[:x[:x[:x[:x[:x[:x[:x[:x[
:x{:x{:x{:x{:x{:x{:x{:x{:x{:x{:x{:x{:x{:x{:x{:x{:x{:x{:x{:x{
::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
