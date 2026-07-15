# Releasing Markdown Core

Markdown Core publishes C, SwiftPM, Kotlin Multiplatform, and npm artifacts from
one protected Git commit and one version in `VERSION`. The first release is
`1.0.0`. The repository does not inherit old cmark-gfm tags, and it does not
promise C binary ABI compatibility between releases. Consumers must rebuild the
C engine and bindings together for each release.

## One-time repository and registry setup

Create a GitHub environment named `release`. Require at least one reviewer,
prevent self-review where the organization plan supports it, and restrict the
environment to protected tags matching `v*.*.*`. Configure a tag ruleset that
prevents unreviewed creation, update, and deletion of release tags. The formal
workflow is `.github/workflows/release.yml`; it has no manual or pull-request
trigger and rejects a tag that is not exactly `v<VERSION>` or whose commit is
not on `origin/main`.

The repository currently applies the checked-in recipes in
`.github/environments/release.json`,
`.github/environments/release-tag-policy.json`, and
`.github/rulesets/release-tags.json`. `DongyuZhao` is the sole environment
reviewer and release-tag bypass actor. Because the organization currently has
no reviewer team, `prevent_self_review` is intentionally `false`; setting it to
`true` would deadlock the only release operator. When a second release owner or
team exists, replace the user reviewer with that team and enable self-review
prevention before the next release. The environment accepts only `v*.*.*` tags,
and the active tag ruleset restricts matching tag creation, update, and deletion
to the named bypass actor. GitHub currently reports `can_admins_bypass=true`, its
default recovery path; with one administrator this does not grant access to a
second actor, but it must be disabled in the web settings when a separate
reviewer team is established.

The environment contains only these Maven secrets:

| Secret | Purpose |
| --- | --- |
| `MAVEN_CENTRAL_USERNAME` | Username from an expiring Central Portal user token |
| `MAVEN_CENTRAL_PASSWORD` | Password from the same Portal user token |
| `MAVEN_SIGNING_KEY` | ASCII-armored private OpenPGP signing key |
| `MAVEN_SIGNING_PASSWORD` | Passphrase for that private key |

An empty environment secret list is expected until the Portal token and release
key have been created. Do not populate placeholders. Verify the live policy and
secret names without reading values:

```sh
gh api repos/nouprax/markdown-core/environments/release
gh api repos/nouprax/markdown-core/environments/release/deployment-branch-policies
gh api repos/nouprax/markdown-core/rulesets
gh secret list --repo nouprax/markdown-core --env release
```

Generate the Central token from the Portal account page. Record its owner,
creation date, expiry date, and renewal owner in the organization password
manager, never in this repository. Use the verified `com.nouprax` namespace.
The workflow uploads one `USER_MANAGED` deployment, waits for Portal validation,
publishes npm, and only then asks Central to publish the validated deployment.
[Central documents the token authentication and Publisher API lifecycle](https://central.sonatype.org/publish/publish-portal-api/).

Generate a dedicated release signing key with a strong passphrase and a finite
expiry. Store an encrypted offline copy of the private key and revocation
certificate separately from GitHub. Publish the public key to at least two of
Central's supported services (`keyserver.ubuntu.com`, `keys.openpgp.org`, or
`pgp.mit.edu`) and prove it is retrievable before release. Central requires PGP
detached signatures and documents its supported key servers in its
[GPG requirements](https://central.sonatype.org/publish/requirements/gpg/).
The CI copy must never be the only private-key copy.

The current release-signing key was created on 2026-07-14 with UID
`Nouprax Release <nouprax@outlook.com>` and expires on 2028-07-14. Its
fingerprint is `0E46 FE94 9804 A119 20FE 904C 2D6E 1C75 20B9 5B01`.
Retrieval by fingerprint/key ID was verified against `keys.openpgp.org` and
`keyserver.ubuntu.com`; the protected `release` environment contains the
matching private key and passphrase. The encrypted offline private-key and
revocation-certificate backup was confirmed on 2026-07-14.

The npm package is public `@nouprax/es-markdown-core`. If npm requires a first
bootstrap publish before trusted publishing can be configured, use a short-lived,
package-scoped granular token or an interactive 2FA publish. Immediately after
that publish, configure its GitHub Actions trusted publisher with these exact
values:

- organization: `nouprax`
- repository: `markdown-core`
- workflow filename: `release.yml`
- environment: `release`
- allowed action: `npm publish`

Run one OIDC publication, verify the provenance attestation on npm, revoke the
bootstrap token, require 2FA, and disallow traditional token publishing. No npm
write token belongs in GitHub. npm requires a GitHub-hosted runner plus
`contents: read` and `id-token: write`; the release job grants exactly those
permissions. See npm's current [trusted publisher procedure](https://docs.npmjs.com/trusted-publishers/).

## Release preparation

1. Update `VERSION`, the checked-in C version header, package examples, and all
   consumer fixtures. `pnpm release:check-version` rejects drift.
2. Move the matching `CHANGELOG.md` section from `Unreleased` to the release
   date and prepare the human-facing release summary.
3. Confirm the exact commit has successful `Required gates`, `CodeQL gate`, and
   `Release dry-run gate` checks. The formal workflow rechecks the first two by
   commit SHA before it builds anything.
4. Run the cache-cold AGP/Kotlin/Gradle matrix with `--warning-mode=fail`, both
   Android managed devices, all host tests, publication tasks, and consumers.
5. Perform a clean import/sync with the supported IntelliJ IDEA and Android
   Studio versions in `docs/toolchains.md`; no credential may be needed for sync.
6. Review `npm view @nouprax/es-markdown-core versions`, the Maven Central
   coordinates, and `git tag --list`. Versions and release tags are immutable.

The pull-request-capable `.github/workflows/release-dry-run.yml` reads no
environment or repository secrets and has only `contents: read`. It builds C
archives on Linux and macOS, validates the SwiftPM source archive and plugin,
packs/runs the npm consumer, aggregates every KMP publication, merges Linux and
macOS JNI payloads into the one JVM JAR, signs with a disposable PGP key, checks
SHA-256/SHA-512 files, and runs KMP, JVM Gradle, Android, and Maven Wrapper
consumers from the staged repository.

## Publishing

Create the signed protected tag only after all preparation is complete:

```sh
git tag -s v1.0.0 -m "Markdown Core 1.0.0"
git push origin v1.0.0
```

The workflow validates the tag and commit, rebuilds every artifact without a
shared Gradle cache, and then pauses at the protected `release` environment. A
reviewer compares the workflow SHA, tag, changelog, dry-run evidence, key
fingerprint, and token expiry before approval.

Publication ordering is:

1. Build and audit C, SwiftPM, npm, Linux Maven, and macOS Maven artifacts.
2. Aggregate the Maven publication set and the two desktop JNI payloads; sign
   every deployed file; generate SHA-256/SHA-512 files; run staged consumers.
3. Upload a user-managed Central deployment and wait for `VALIDATED`.
4. Publish npm using OIDC. npm generates provenance for the public package.
5. Publish the already validated Central deployment and wait for `PUBLISHED`.
6. Generate GitHub Release checksums, create GitHub artifact attestations, and
   publish the source/C/npm/Central bundle from the same tag.

SwiftPM release identity is derived from
`https://github.com/nouprax/markdown-core` as `markdown-core`; consumers import
the `MarkdownCore` product/module. The Swift source archive intentionally keeps
repo tests, canonical specs, and the build-tool plugin. The release consumer
builds only `MarkdownCore`, does not execute the test plugin, and does not carry
its generated fixture. Installed C, Maven, npm, and compiled Swift product
artifacts exclude the shared spec and private implementation surface.

## Verification and recovery

Download the GitHub Release assets and verify `SHA256SUMS`/`SHA512SUMS`. Use
`gh attestation verify <artifact> --repo nouprax/markdown-core` for GitHub
provenance. Verify Maven `.asc` files against the published key, inspect the
Central deployment's complete coordinate set, run the repository consumers,
and confirm npm shows provenance tied to `release.yml` and the release commit.

If Central validation fails before npm publishing, leave the failed deployment
available while diagnosing it, or explicitly drop it after evidence is saved.
Do not move or reuse the release tag. If npm succeeds but Central publication
later fails, do not republish npm or mutate artifacts; preserve the Central
deployment, fix only external availability/authorization, and resume publication
of those same signed bytes. Any byte-level correction requires a new SemVer.

Rotate a Portal token before expiry or whenever an owner/workflow changes.
Replace both environment values atomically and revoke the old token after a
successful dry authentication check. To rotate a PGP key, publish the new public
key first, confirm retrieval, update the two signing secrets atomically, run a
dry signature verification, and retain/revoke the prior key according to the
recorded transition date.

For a suspected leak, disable the `release` environment and tag creation first.
Revoke the Portal/bootstrap token, remove any npm traditional token, revoke the
PGP key and publish its revocation certificate, rotate affected credentials,
audit workflow and registry logs, and document affected immutable versions.
Never delete evidence or silently overwrite a published version.
