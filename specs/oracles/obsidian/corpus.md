# Obsidian oracle corpus

These are comparison inputs, not product goldens. The empty expected half is
intentional: `scripts/check-obsidian-parity.mjs` parses the input with both
implementations and never reads an expected AST from this file.

## Inherited link destinations

```````````````````````````````` example
[local](#target)
.
````````````````````````````````

```````````````````````````````` example
![alt](image.png "title")
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

```````````````````````````````` example
%%only%%
.
````````````````````````````````

```````````````````````````````` example
> a %%b
> c%% d ==e== [[f]]
.
````````````````````````````````

## Custom task characters

```````````````````````````````` example
- [?] custom state
.
````````````````````````````````

```````````````````````````````` example
- [ ] open
- [x] done
- [X] uppercase
- [-] dash
- [é] two
- [✓] three
.
````````````````````````````````

```````````````````````````````` example
> 1. [?] outer
>    - [✓] inner
.
````````````````````````````````

```````````````````````````````` example
- [🚀] supplementary scalar
.
````````````````````````````````

```````````````````````````````` example
- []] closing bracket
.
````````````````````````````````

```````````````````````````````` example
- [?] nonstructural separator
.
````````````````````````````````

```````````````````````````````` example
- [?] →  body
.
````````````````````````````````

```````````````````````````````` example
- [?] # heading
.
````````````````````````````````

```````````````````````````````` example
- [\?] escaped candidate
.
````````````````````````````````

```````````````````````````````` example
-
  [?] later first block
.
````````````````````````````````

```````````````````````````````` example
- [] empty
- [ab] multiple
- [é] decomposed
- [?]none
- [?]
.
````````````````````````````````
