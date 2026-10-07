# Releasing docuconf-cpp

C++ has no central package registry, so a release is a git tag plus a GitHub release with a source tarball.
Consumers use `FetchContent` (pinned to the tag) or install the library and use `find_package(docuconf)`.

Releases are automated with [release-please](https://github.com/googleapis/release-please); see
[CONTRIBUTING.md](CONTRIBUTING.md#how-releases-happen) for the commit conventions it reads.

## Each release

1. Merge the open release PR (`chore(main): release X.Y.Z`). It already bumps `VERSION` in the `project()` call in
   `CMakeLists.txt` (also the `metadata.generator.version` of exported contracts) and updates `CHANGELOG.md`. The
   golden file and the example contract do not need regenerating: their comparisons ignore the generator version.
2. release-please tags the merge commit `vX.Y.Z` and creates the GitHub release with the changelog entries.
3. `.github/workflows/release.yml` runs on the tag. It checks that the tag matches the CMake version, builds
   `docuconf-cpp-<version>.tar.gz` with `git archive`, and attaches the tarball and its SHA-256 to the release.
   It uses the workflow's own `GITHUB_TOKEN`; no secret is needed.

If the release PR was created with `GITHUB_TOKEN` (no release GitHub App configured), the tag does not trigger
`release.yml` by itself, so `.github/workflows/release-please.yml` starts it with `gh workflow run`. To redo a
release by hand: `gh workflow run release.yml --ref vX.Y.Z`.

## Later: vcpkg and Conan

Submitting to package managers is a separate step, done once a release exists:

- **vcpkg:** add a port to [microsoft/vcpkg](https://github.com/microsoft/vcpkg) (`ports/docuconf/portfile.cmake`
  with `vcpkg_from_github` on the release tag and its SHA-512, plus `vcpkg.json` depending on `cli11`,
  `nlohmann-json`, `re2`, `openssl`, `yaml-cpp`, `tomlplusplus` and `json-schema-validator`), then open a pull
  request there.
- **Conan:** add a recipe to [conan-center-index](https://github.com/conan-io/conan-center-index)
  (`recipes/docuconf/all/conanfile.py` and `conandata.yml` pointing at the release tarball and its SHA-256).

Both build with `-DDOCUCONF_FETCH_DEPS=OFF`, so every dependency comes from the package manager.

The project is MIT licensed (`LICENSE`).
