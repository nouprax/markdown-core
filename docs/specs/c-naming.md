# C naming conventions

Status: adopted 2026-07-16 (unification pass over the engine, extensions,
tests, fuzz targets, and CLI; verified by `nm` over the installed archives).
Applies to every C symbol in `packages/markdown-core`. The public facade
surface (`include/markdown_core.h` + export allowlists) additionally goes
through a naming freeze review before the 2.0.0 release.

## Rules

1. **Prefix.** Every symbol with external linkage — including
   engine-internal helpers that merely cross translation units — is spelled
   `markdown_core_…`. No exceptions: unprefixed cross-TU symbols leak into
   consumer link namespaces when static-linking, and leading-underscore
   names are reserved identifiers. (This retired the inherited `_scan_*`,
   `_ext_scan_at`, `houdini_*`, `normalize_map_label`, and `mkc_*` families.)
2. **Subject first.** Functions are `markdown_core_<subject>_<verb>[_<object>]`,
   where the subject is the type the function operates on or produces:
   `markdown_core_node_parse_document`, `markdown_core_extension_find`,
   `markdown_core_map_normalize_label`. Attribute accessors that come in
   pairs keep `get_`/`set_`; navigation and singleton accessors are bare
   nouns (`markdown_core_node_next`, `markdown_core_mem_default`).
3. **Function-pointer typedefs** end in `_func` and are named after the
   descriptor field they type: field `match_inline` ↔ typedef
   `markdown_core_match_inline_func`, field `alloc_opaque` ↔
   `markdown_core_alloc_opaque_func`.
4. **Extension descriptor fields** are verb-object phrases with no suffix:
   `try_opening_block`, `match_inline`, `postprocess_block`, `can_contain`,
   `accepts_lines`, `alloc_opaque`. Never `…_func` on a field.
5. **File-local statics** are free-form (the `S_` prefix is a common engine
   idiom, not a requirement); they carry no prefix obligation because they
   have no linkage.
6. **Types** follow the same prefix rule (`markdown_core_id_table`); no
   abbreviation prefixes (`mkc_`) even in internal headers.

## Facade accessor prefixes (decided 2026-07-16)

The public facade (`include/markdown_core.h`) keeps `get_` on exactly one
cluster — node identity/traversal and error accessors (`node_get_id`,
`node_get_kind`, `node_get_revision`, `node_get_parent`,
`node_get_first_child`, `node_get_next_sibling`, `error_get_*`) — and uses
bare `subject_attribute` names everywhere else (kind-specific property
accessors, `session_*`, `changeset_*`, `document_root`). This is
deliberate, not drift: the bare names for that cluster are occupied
(`markdown_core_node_id` and `markdown_core_node_kind` are type names, and
`node_first_child`/`node_parent` are the raw internal traversal functions),
and removing `get_` only where possible would split the cluster. Do not
re-litigate at the M4 freeze review unless the occupying names change.

## Non-goals

Historical migration documents keep the names that were current when they
were written. Generated scanners (`scanners.c`, `ext_scanners.c`) are edited
together with their `.re` sources; re2c is not run at build time.
