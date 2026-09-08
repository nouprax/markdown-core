---
# source
base: &a 9007199254740993
copy: *a
list: [one, 2]
...
not YAML
bad: [true]
base: duplicate
root: {nested: unsupported}
after: present
---
Body **outside** metadata.
