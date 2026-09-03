# Releasing Markdown Core

Every C, Swift, Kotlin, and ECMAScript artifact in one release is built from
one protected commit and one stable semantic version. The root `VERSION` file
is the only machine-readable release-version input. Markdown is deliberately
not parsed to determine artifact versions.

The human-readable counterpart is `docs/releases/<VERSION>.md`. Its first line
must be `# Markdown Core <VERSION>`. The release workflow uses that file as the
GitHub Release body, but the file cannot override `VERSION`.

## Version contract

Changing `VERSION` prepares a new coordinated release. Before a tag can build,
`pnpm release:check-version` verifies all required projections:

- the tag is exactly `v<VERSION>`;
- CMake and Gradle derive their publication versions from `VERSION`;
- the npm manifest, checked-in C version header, consumer fixtures, README
  examples, and changelog agree with it;
- `docs/releases/<VERSION>.md` exists and has the matching heading; and
- no existing release tag collides with or succeeds the prepared version.

SwiftPM has no version field in `Package.swift`; its package version is the Git
tag. The same exact tag therefore identifies the Swift package while C, Maven,
npm, and archive filenames carry the value read from `VERSION`.

Do not add another version manifest or make CI extract a number from prose. An
ecosystem file that must repeat the version is a checked projection and may not
become an independent source of truth.

## Prepare a release

1. Set `VERSION` to the intended stable SemVer.
2. Update the checked projections named by `scripts/check-release-version.mjs`.
3. Move the matching changelog section from `unreleased` to its release date.
4. Write `docs/releases/<VERSION>.md` for consumers. Keep internal migration
   bookkeeping and validation transcripts out of the release notes.
5. Run `pnpm release:check-version`, `pnpm verify`, and the release dry run.
6. Review the produced C archives, Swift source archive, npm package, Maven
   repository, checksums, and their staged consumers.

The local dry run is:

```sh
pnpm release:dry-run
```

The `Release Dry Run` workflow performs the cross-host Linux/macOS aggregation
without access to publication credentials.

## Publish

Create the signed protected tag only after preparation has passed:

```sh
version=$(cat VERSION)
git tag -s "v$version" -m "Markdown Core $version"
git push origin "v$version"
```

The tag-triggered release validates the tag and version contract before it
builds anything. It then runs the normal quality workflow, constructs and
tests every artifact, uploads a user-managed Maven Central deployment, publishes
npm through its GitHub trusted publisher, commits the validated Central
deployment, and creates the GitHub Release from the same immutable artifacts.

The protected `release` environment contains only the Maven Central username
and password plus the signing key and passphrase. npm publication uses OIDC and
must not use a repository token. GitHub Release artifacts receive checksums and
build-provenance attestations.

## Recovery

Published versions and release tags are immutable. A byte-level correction
requires a new SemVer.

If a release stops after the Central deployment was created, use the release
workflow's manual resume inputs for the existing tag and source run. The resume
job verifies that the run, commit, tag, recorded version, Central deployment,
and downloaded package versions all agree before it continues. It treats an
already published npm package or Central deployment as completed work rather
than rebuilding or republishing different bytes.
