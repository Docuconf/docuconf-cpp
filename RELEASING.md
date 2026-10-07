# Releasing docuconf-cpp

C++ has no central package registry, so a release is a git tag plus a GitHub release with a source tarball.
Consumers use `FetchContent` (pinned to the tag) or install the library and use `find_package(docuconf)`.

Releases are automated with [release-please](https://github.com/googleapis/release-please); see
[CONTRIBUTING.md](CONTRIBUTING.md#how-releases-happen) for the commit conventions it reads.

## Each release

1. Make sure CI is green on `main`, including conformance and `cue vet`.
2. Merge the open release PR (`chore(main): release X.Y.Z`). It already bumps `VERSION` in the `project()` call in
   `CMakeLists.txt` (also the `metadata.generator.version` of exported contracts), the version in `README.md`, and
   updates `CHANGELOG.md`. The golden file and the example contract do not need regenerating: their comparisons
   ignore the generator version.
3. release-please tags the merge commit `vX.Y.Z` and creates the GitHub release with the changelog entries.
4. `.github/workflows/release.yml` runs on the tag. It checks that the tag matches the CMake version, builds
   `docuconf-cpp-<version>.tar.gz` with `git archive`, and attaches the tarball and its SHA-256 to the release.
   It uses the workflow's own `GITHUB_TOKEN`; no secret is needed.

**First release only.** Until a tag exists, `README.md` must install with `GIT_TAG main`. Just before merging the
first release PR, land a commit on `main` that changes the install block to
`GIT_TAG v0.1.0)  # x-release-please-version` (the release PR then rewrites the version to its own) and drops the
"not released yet" status and the "no release tag yet" note. From then on the release PR keeps both version
mentions in the README current.

The manual steps of the pre-release-please process are gone: release-please bumps the CMake version and writes the
changelog, the golden file and the example contract no longer need regenerating because their checks ignore only
`metadata.generator.version`, and release-please creates the tag instead of `git tag` and `git push`.

If the release PR was created with `GITHUB_TOKEN` (no release GitHub App configured), the tag does not trigger
`release.yml` by itself, so `.github/workflows/release-please.yml` starts it with `gh workflow run`. To redo a
release by hand: `gh workflow run release.yml --ref vX.Y.Z`.

## Later: vcpkg and Conan

Submitting to package managers is a separate step, done once a release exists:

- **vcpkg:** add a port to [microsoft/vcpkg](https://github.com/microsoft/vcpkg) (`ports/docuconf/portfile.cmake`
  with `vcpkg_from_github` on the release tag and its SHA-512, plus `vcpkg.json` depending on `cli11`,
  `nlohmann-json`, `re2`, `openssl`, `yaml-cpp`, `tomlplusplus` and `json-schema-validator`, with a
  `files` feature mapping to `DOCUCONF_FILE_INPUTS`), then open a pull request there.
- **Conan:** add a recipe to [conan-center-index](https://github.com/conan-io/conan-center-index)
  (`recipes/docuconf/all/conanfile.py` and `conandata.yml` pointing at the release tarball and its SHA-256).

Both build with `-DDOCUCONF_FETCH_DEPS=OFF`, so every dependency comes from the package manager. That is also
what the install rules need: a build that fetched CLI11, nlohmann/json, RE2, yaml-cpp or toml++ generates no
install rules (only a fetched json-schema-validator is installed alongside docuconf), because the installed
`docuconfConfig.cmake` finds its dependencies with `find_package`.

The project is MIT licensed (`LICENSE`).

## docuconf-go version

The spec, the CUE meta-schema and the shared conformance suite live in
[docuconf-go](https://github.com/Docuconf/docuconf-go). `.github/docuconf-go.ref` holds the full docuconf-go commit SHA
this SDK is tested against.

- **Push and pull request CI** check out docuconf-go at that commit, so a change in docuconf-go never breaks this
  repository's CI by surprise.
- **Bump pull requests.** `.github/workflows/docuconf-go-bump.yml` opens (or updates) a
  `build(deps): bump docuconf-go to <sha>` pull request on the `docuconf-go-bump` branch whenever docuconf-go's `main`
  moves: on a `docuconf-go-updated` dispatch from docuconf-go, and daily as a catch-up. CI on that pull request is the
  compatibility check; merge it when it is green. Run the workflow by hand (optionally with a `sha`) to pin a
  specific commit.
- **Nightly.** CI also runs every night against docuconf-go `main`, and can be started by hand with a
  `docuconf_go_ref` input to try any branch or commit.
- **`scripts/conformance.sh`** runs just the shared conformance suite and the `cue vet` tests against a docuconf-go
  checkout: `DOCUCONF_GO_DIR=../docuconf-go scripts/conformance.sh`. docuconf-go runs it on every pull request that
  touches the spec, so a breaking spec change shows up there before it merges.

Without the release GitHub App (`RELEASE_APP_ID` and `RELEASE_APP_PRIVATE_KEY`), the bump pull request is created with
`GITHUB_TOKEN`, which starts no workflows, so the bump workflow starts CI on the branch itself. That needs
**Settings → Actions → General → Allow GitHub Actions to create and approve pull requests**.
