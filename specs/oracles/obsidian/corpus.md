# Obsidian oracle corpus

These are comparison inputs, not product goldens. The empty expected half is
intentional: `scripts/check-obsidian-parity.mjs` parses the input with both
implementations and never reads an expected AST from this file.

## Inherited link destinations

```````````````````````````````` example
[local](#target)
.
````````````````````````````````

## Wikilinks and embeds

```````````````````````````````` example
[[Note]]
.
````````````````````````````````

```````````````````````````````` example
[[Folder/Note#Heading#Child|Display text]]
.
````````````````````````````````

```````````````````````````````` example
![[Image.png|100x145]]
.
````````````````````````````````

```````````````````````````````` example
![[Note#^block-id]]
.
````````````````````````````````

## Highlights and comments

```````````````````````````````` example
before ==marked== after
.
````````````````````````````````

```````````````````````````````` example
before %%hidden%% after
.
````````````````````````````````

```````````````````````````````` example
%%
hidden **strong**
%%
.
````````````````````````````````

## Custom task characters

```````````````````````````````` example
- [?] custom state
.
````````````````````````````````
